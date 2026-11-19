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

use harness::oid::*;
use harness::pgx::Time;
use harness::plan::PlanFact;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_copy() {
    run_scripts(&[
        ScriptTest {
            name: "tab delimited with header",
            set_up_script: &[
                "CREATE TABLE test (pk int primary key);",
                "INSERT INTO test VALUES (0), (1);",
                "CREATE TABLE test_info (id int, info varchar(255), test_pk int, primary key(id), foreign key (test_pk) references test(pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test_info FROM STDIN WITH (HEADER);",
                    expected: Expected::Tag("COPY 3"),
                    copy_from_stdin_file: "tab-load-with-header.sql",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test_info order by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("info", VARCHAR), Column("test_pk", INT4)],
                        rows: &[
                            &[T("4"), T("string for 4"), T("1")],
                            &[T("5"), T("string for 5"), T("0")],
                            &[T("6"), T("string for 6"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "tab delimited with header and column names",
            set_up_script: &[
                "CREATE TABLE test (pk int primary key);",
                "INSERT INTO test VALUES (0), (1);",
                "CREATE TABLE test_info (id int, info varchar(255), test_pk int, primary key(id), foreign key (test_pk) references test(pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test_info (id, info, test_pk) FROM STDIN WITH (HEADER);",
                    expected: Expected::Tag("COPY 3"),
                    copy_from_stdin_file: "tab-load-with-header.sql",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test_info order by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("info", VARCHAR), Column("test_pk", INT4)],
                        rows: &[
                            &[T("4"), T("string for 4"), T("1")],
                            &[T("5"), T("string for 5"), T("0")],
                            &[T("6"), T("string for 6"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "tab delimited with quoted column names",
            set_up_script: &[
                r#"CREATE TABLE Regions (
   "Id" SERIAL UNIQUE NOT NULL,
   "Code" VARCHAR(4) UNIQUE NOT NULL,
   "Capital" VARCHAR(10) NOT NULL,
   "Name" VARCHAR(255) UNIQUE NOT NULL
);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"COPY regions ("Id", "Code", "Capital", "Name") FROM stdin;
"#,
                    expected: Expected::Tag("COPY 3"),
                    copy_from_stdin_file: "tab-load-with-quoted-column-names.sql",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "timestamp columns",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk timestamp primary key, ts timestamp);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl1 FROM STDIN WITH (HEADER)",
                    expected: Expected::Tag("COPY 3"),
                    copy_from_stdin_file: "tab-load-with-timestamp-col.sql",
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from tbl1 order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", TIMESTAMP), Column("ts", TIMESTAMP)],
                        rows: &[
                            &[T("2020-12-19 19:00:00"), T("2021-04-04 20:00:00")],
                            &[T("2020-12-19 21:36:32.188"), T("2020-12-19 19:00:00")],
                            &[T("2021-04-04 20:00:00"), T("2020-12-19 21:36:32.188")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic csv",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int primary key, c1 varchar(100), c2 varchar(250));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl1 FROM STDIN (FORMAT CSV)",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "\.""#, ..E }),
                    copy_from_stdin_file: "csv-load-basic-cases.sql",
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from tbl1 where pk = 6 order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from tbl1 where pk = 9;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "csv with header",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int primary key, c1 varchar(100), c2 varchar(250));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: " COPY tbl1 FROM STDIN (FORMAT CSV, HEADER TRUE);",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "\.""#, ..E }),
                    copy_from_stdin_file: "csv-load-with-header.sql",
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from tbl1 where pk = 6 order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "generated column",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int primary key, c1 varchar(100), c2 varchar(250), c3 int generated always as (pk + 10) stored);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl1 (pk, c1, c2) FROM STDIN (FORMAT CSV)",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "\.""#, ..E }),
                    copy_from_stdin_file: "csv-load-basic-cases.sql",
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from tbl1 where pk = 6 order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR), Column("c3", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from tbl1 where pk = 9;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR), Column("c3", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "load multiple chunks",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int primary key, c1 varchar(100), c2 varchar(250));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl1 FROM STDIN (FORMAT CSV);",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "\.""#, ..E }),
                    copy_from_stdin_file: "csv-load-multi-chunk.sql",
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from tbl1 where pk = 99 order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "load psv with headers",
            set_up_script: &[
                "CREATE TABLE test (pk int primary key);",
                "INSERT INTO test VALUES (0), (1);",
                "CREATE TABLE test_info (id int, info varchar(255), test_pk int, primary key(id), foreign key (test_pk) references test(pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test_info FROM STDIN (FORMAT CSV, HEADER TRUE, DELIMITER '|');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "\.""#, ..E }),
                    copy_from_stdin_file: "psv-load.sql",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test_info order by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("info", VARCHAR), Column("test_pk", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "csv from file",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int primary key, c1 varchar(100), c2 varchar(250));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl1 FROM '{TESTDATA}/csv-load-basic-cases.sql' (FORMAT CSV)",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "\.""#, ..E }),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from tbl1 where pk = 6 order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from tbl1 where pk = 9;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "csv from file with column names",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int primary key, c1 varchar(100), c2 varchar(250));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl1 (pk, c1, c2) FROM '{TESTDATA}/csv-load-basic-cases.sql' (FORMAT CSV)",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "\.""#, ..E }),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from tbl1 where pk = 6 order by pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from tbl1 where pk = 9;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "tab delimited with header from file",
            set_up_script: &[
                "CREATE TABLE test (pk int primary key);",
                "INSERT INTO test VALUES (0), (1);",
                "CREATE TABLE test_info (id int, info varchar(255), test_pk int, primary key(id), foreign key (test_pk) references test(pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test_info FROM '{TESTDATA}/tab-load-with-header.sql' WITH (HEADER)",
                    expected: Expected::Tag("COPY 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test_info order by 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("info", VARCHAR), Column("test_pk", INT4)],
                        rows: &[
                            &[T("4"), T("string for 4"), T("1")],
                            &[T("5"), T("string for 5"), T("0")],
                            &[T("6"), T("string for 6"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "tab delimited with uuid values",
            set_up_script: &[
                r#"CREATE TABLE public.uuid_table (
    id uuid NOT NULL,
    name character varying NOT NULL,
    second_uuid uuid DEFAULT '428d0815-d95b-4cfc-89af-9fca38585dcc'::uuid);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY uuid_table (id, name, second_uuid) FROM STDIN",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "uuid-table.sql",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM uuid_table order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", UUID), Column("name", VARCHAR), Column("second_uuid", UUID)],
                        rows: &[
                            &[T("1077f506-a6fc-4cb2-aed2-9dea9351ed9c"), T("Company A"), T("428d0815-d95b-4cfc-89af-9fca38585dcc")],
                            &[T("5e080b3a-361f-4e16-b7a4-70d4f175e283"), T("Company B"), T("428d0815-d95b-4cfc-89af-9fca38585dcc")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "binary from stdin",
            set_up_script: &[
                "CREATE TABLE tbl3 (pk int primary key, c1 text, b boolean);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl3 FROM STDIN (FORMAT BINARY);",
                    expected: Expected::Tag("COPY 4"),
                    copy_from_stdin_file: "copy-to-basic.bin",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tbl3 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT), Column("b", BOOL)],
                        rows: &[
                            &[T("1"), T("foo"), T("t")],
                            &[T("2"), Null, T("f")],
                            &[T("3"), T(""), Null],
                            &[T("4"), T("héllo"), T("t")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "binary load multiple chunks",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int primary key, c1 text);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl1 FROM STDIN (FORMAT BINARY);",
                    expected: Expected::Tag("COPY 2000"),
                    copy_from_stdin_file: "binary-load-multi-chunk.bin",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), count(c1), sum(length(c1)) FROM tbl1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("2000"), T("1980"), T("202536")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, length(c1) FROM tbl1 WHERE pk IN (99, 211, 300, 1999) ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("99"), T("99")],
                            &[T("211"), T("0")],
                            &[T("300"), Null],
                            &[T("1999"), T("100")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "binary from file",
            set_up_script: &[
                "CREATE TABLE tbl3 (pk int primary key, c1 text, b boolean);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl3 FROM '{TESTDATA}/copy-to-basic.bin' (FORMAT BINARY);",
                    expected: Expected::Tag("COPY 4"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tbl3 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT), Column("b", BOOL)],
                        rows: &[
                            &[T("1"), T("foo"), T("t")],
                            &[T("2"), Null, T("f")],
                            &[T("3"), T(""), Null],
                            &[T("4"), T("héllo"), T("t")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "malformed binary load does not poison the session",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int primary key, c1 text);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl1 FROM STDIN (FORMAT BINARY);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "row field count is 5, expected 2", ..E }),
                    copy_from_stdin_file: "binary-load-malformed.bin",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM tbl1;",
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
                    query: "INSERT INTO tbl1 VALUES (100, 'still works');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY tbl1 FROM STDIN (FORMAT BINARY);",
                    expected: Expected::Tag("COPY 3"),
                    copy_from_stdin_file: "binary-load-2col.bin",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tbl1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), Null],
                            &[T("3"), T("three")],
                            &[T("100"), T("still works")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "binary load failing after a successful chunk does not poison the session",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int primary key, c1 text);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl1 FROM STDIN (FORMAT BINARY);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "row field count is 5, expected 2", ..E }),
                    copy_from_stdin_file: "binary-load-malformed-late.bin",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM tbl1;",
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
                    query: "COPY tbl1 FROM STDIN (FORMAT BINARY);",
                    expected: Expected::Tag("COPY 3"),
                    copy_from_stdin_file: "binary-load-2col.bin",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM tbl1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "binary load missing its trailer does not poison the session",
            set_up_script: &[
                "CREATE TABLE tbl3 (pk int primary key, c1 text, b boolean);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl3 FROM STDIN (FORMAT BINARY);",
                    expected: Expected::Tag("COPY 4"),
                    flow: Flow::Exec,
                    copy_from_stdin_file: "copy-from-missing-trailer.bin",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM tbl3;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY tbl3 FROM STDIN (FORMAT BINARY);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "tbl3_pkey""#, detail: "Key (pk)=(1) already exists.", schema: "public", table: "tbl3", constraint: "tbl3_pkey", ..E }),
                    flow: Flow::Query,
                    copy_from_stdin_file: "copy-to-basic.bin",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tbl3 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT), Column("b", BOOL)],
                        rows: &[
                            &[T("1"), T("foo"), T("t")],
                            &[T("2"), Null, T("f")],
                            &[T("3"), T(""), Null],
                            &[T("4"), T("héllo"), T("t")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "binary errors",
            set_up_script: &[
                "CREATE TABLE tbl3 (pk int primary key, c1 text, b boolean);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl3 FROM STDIN (FORMAT BINARY, HEADER);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot specify HEADER in BINARY mode", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY tbl3 FROM STDIN (FORMAT BINARY, DELIMITER '|');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "cannot specify DELIMITER in BINARY mode", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY tbl3 FROM '{TESTDATA}/copy-to-basic.txt' (FORMAT BINARY);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "COPY file signature not recognized", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY tbl3 FROM '{TESTDATA}/copy-from-missing-trailer.bin' (FORMAT BINARY);",
                    expected: Expected::Tag("COPY 4"),
                    flow: Flow::Exec,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "file not found",
            set_up_script: &[
                "CREATE TABLE test (pk int primary key);",
                "INSERT INTO test VALUES (0), (1);",
                "CREATE TABLE test_info (id int, info varchar(255), test_pk int, primary key(id), foreign key (test_pk) references test(pk));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY test_info FROM '{TESTDATA}/file-not-found.sql' WITH (HEADER)",
                    expected: Expected::Error(Diagnostic { code: "58P01", message: r#"could not open file "{TESTDATA}/file-not-found.sql" for reading: No such file or directory"#, hint: r#"COPY FROM instructs the PostgreSQL server process to read a file. You may want a client-side facility such as psql's \copy."#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "wrong columns",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int primary key, c1 varchar(100), c2 varchar(250));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl1 (pk, c1) FROM '{TESTDATA}/csv-load-basic-cases.sql' (FORMAT CSV)",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "extra data after last expected column", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY tbl1 (pk, c1, c3) FROM '{TESTDATA}/csv-load-basic-cases.sql' (FORMAT CSV)",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "c3" of relation "tbl1" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "table not found",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int primary key, c1 varchar(100), c2 varchar(250));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl2 (pk, c1) FROM '{TESTDATA}/csv-load-basic-cases.sql' (FORMAT CSV)",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "tbl2" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "read only table",
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so a Postgres-flavored error is expected.
                ScriptTestAssertion {
                    query: "COPY dolt_log FROM '{TESTDATA}/csv-load-basic-cases.sql' (FORMAT CSV)",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"cannot change relation "dolt_log""#, ..E }),
                    skip: Some("Dolt system tables do not resolve as the targets of writes yet"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "bad data rows",
            set_up_script: &[
                "CREATE TABLE tbl1 (pk int primary key, c1 varchar(100), c2 varchar(250));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY tbl1 (pk, c1, c2) FROM '{TESTDATA}/missing-columns.sql' (FORMAT CSV)",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: r#"missing data for column "c2""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select count(*) from tbl1;",
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
                    query: "COPY tbl1 (pk, c1, c2) FROM '{TESTDATA}/too-many-columns.sql' (FORMAT CSV)",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "extra data after last expected column", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select count(*) from tbl1;",
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
                    query: "COPY tbl1 (pk, c1, c2) FROM '{TESTDATA}/wrong-types.sql' (FORMAT CSV)",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "abc""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select count(*) from tbl1;",
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
    ]);
}

#[test]
fn test_copy_rules() {
    run_scripts(&[
        ScriptTest {
            name: "copy option errors",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk INT PRIMARY KEY, c1 TEXT, c2 TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (FORMAT CSV, QUOTE 'ab');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "COPY quote must be a single one-byte character", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (QUOTE 'a');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "COPY QUOTE requires CSV mode", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (ESCAPE 'a');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "COPY ESCAPE requires CSV mode", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (bogus 1);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"option "bogus" not recognized"#, position: 19, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COPY t TO STDOUT (FORMAT CSV, DELIMITER '"');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: "COPY delimiter and quote must be different", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (DELIMITER 'ab');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "COPY delimiter must be a single one-byte character", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (DELIMITER 'a');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"COPY delimiter cannot be "a""#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (FORMAT BINARY, NULL 'x');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "cannot specify NULL in BINARY mode", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (FORMAT CSV, HEADER 'match');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cannot use "match" with HEADER in COPY TO"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (FORMAT CSV, HEADER 'maybe');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"header requires a Boolean value or "match""#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (FORCE_QUOTE *);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "COPY FORCE_QUOTE requires CSV mode", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT CSV, FORCE_QUOTE (c1));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "COPY FORCE_QUOTE cannot be used with COPY FROM", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORCE_NOT_NULL (c1));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "COPY FORCE_NOT_NULL requires CSV mode", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (FORMAT CSV, FORCE_NOT_NULL (c1));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "COPY FORCE_NOT_NULL cannot be used with COPY TO", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (FORMAT CSV, FORCE_NULL (c1));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "COPY FORCE_NULL cannot be used with COPY TO", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (FORMAT CSV, NULL 'a,b');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "COPY delimiter character must not appear in the NULL specification", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COPY t TO STDOUT (FORMAT CSV, NULL 'a"b');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: "CSV quote character must not appear in the NULL specification", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (FORMAT CSV, FORMAT TEXT);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "conflicting or redundant options", position: 31, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t (pk, pk) TO STDOUT;",
                    expected: Expected::Error(Diagnostic { code: "42701", message: r#"column "pk" specified more than once"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t (pk, c3) TO STDOUT;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "c3" of relation "t" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY missing TO STDOUT;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "missing" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "copy round trips through files",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE src (pk INT PRIMARY KEY, c1 TEXT, c2 TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO src VALUES (1, 'a,b', NULL), (2, '', 'x"y'), (3, E'tab\there\\back', E'nl\nx'), (4, '\.', 'q|r');"#,
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COPY src TO '{TEMPDIR}/doltgrust-kept-copy.csv' (FORMAT CSV, HEADER, FORCE_QUOTE (c2), ESCAPE '\');"#,
                    expected: Expected::Tag("COPY 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dst1 (pk INT PRIMARY KEY, c1 TEXT, c2 TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COPY dst1 FROM '{TEMPDIR}/doltgrust-kept-copy.csv' (FORMAT CSV, HEADER, ESCAPE '\');"#,
                    expected: Expected::Tag("COPY 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dst1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT), Column("c2", TEXT)],
                        rows: &[
                            &[T("1"), T("a,b"), Null],
                            &[T("2"), T(""), T(r#"x"y"#)],
                            &[T("3"), T(r#"tab	here\back"#), T(r#"nl
x"#)],
                            &[T("4"), T(r#"\."#), T("q|r")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY src TO '{TEMPDIR}/doltgrust-kept-copy.txt' (DELIMITER '|', NULL 'NULL');",
                    expected: Expected::Tag("COPY 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dst2 (pk INT PRIMARY KEY, c1 TEXT, c2 TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY dst2 FROM '{TEMPDIR}/doltgrust-kept-copy.txt' (DELIMITER '|', NULL 'NULL');",
                    expected: Expected::Tag("COPY 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dst2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT), Column("c2", TEXT)],
                        rows: &[
                            &[T("1"), T("a,b"), Null],
                            &[T("2"), T(""), T(r#"x"y"#)],
                            &[T("3"), T(r#"tab	here\back"#), T(r#"nl
x"#)],
                            &[T("4"), T(r#"\."#), T("q|r")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT pk, c1 FROM src WHERE pk > 1 ORDER BY pk) TO '{TEMPDIR}/doltgrust-kept-copy.bin' (FORMAT BINARY);",
                    expected: Expected::Tag("COPY 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dst3 (pk INT, c1 TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY dst3 FROM '{TEMPDIR}/doltgrust-kept-copy.bin' (FORMAT BINARY);",
                    expected: Expected::Tag("COPY 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dst3 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT)],
                        rows: &[
                            &[T("2"), T("")],
                            &[T("3"), T(r#"tab	here\back"#)],
                            &[T("4"), T(r#"\."#)],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY dst3 FROM '{TEMPDIR}/doltgrust-kept-copy.csv' (FORMAT BINARY);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "COPY file signature not recognized", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY dst1 FROM '{TEMPDIR}/doltgrust-kept-copy.bin' (FORMAT BINARY);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "row field count is 2, expected 3", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "copy null handling in CSV",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE n (pk INT PRIMARY KEY, a TEXT, b TEXT, c TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT 1, '', NULL::TEXT, 'x' UNION ALL SELECT 2, NULL, '', 'y') TO '{TEMPDIR}/doltgrust-kept-nulls.csv' (FORMAT CSV);",
                    expected: Expected::Tag("COPY 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY n FROM '{TEMPDIR}/doltgrust-kept-nulls.csv' (FORMAT CSV);",
                    expected: Expected::Tag("COPY 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, a IS NULL, a, b IS NULL, b, c FROM n ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("?column?", BOOL), Column("a", TEXT), Column("?column?", BOOL), Column("b", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("f"), T(""), T("t"), Null, T("x")],
                            &[T("2"), T("t"), Null, T("f"), T(""), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM n;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY n FROM '{TEMPDIR}/doltgrust-kept-nulls.csv' (FORMAT CSV, FORCE_NOT_NULL (b), FORCE_NULL (a));",
                    expected: Expected::Tag("COPY 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, a IS NULL, a, b IS NULL, b, c FROM n ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("?column?", BOOL), Column("a", TEXT), Column("?column?", BOOL), Column("b", TEXT), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("t"), Null, T("f"), T(""), T("x")],
                            &[T("2"), T("t"), Null, T("f"), T(""), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "copy of generated columns and views",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE g (a INT PRIMARY KEY, b INT GENERATED ALWAYS AS (a * 2) STORED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO g (a) VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY g (b) TO STDOUT;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: r#"column "b" is a generated column"#, detail: "Generated columns cannot be used in COPY.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY g TO '{TEMPDIR}/doltgrust-kept-generated.txt';",
                    expected: Expected::Tag("COPY 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM g;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY g FROM '{TEMPDIR}/doltgrust-kept-generated.txt';",
                    expected: Expected::Tag("COPY 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM g ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT 1 AS x;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY v TO STDOUT;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"cannot copy from view "v""#, hint: "Try the COPY (SELECT ...) TO variant.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY v FROM STDIN;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"cannot copy to view "v""#, hint: "To enable copying to a view, provide an INSTEAD OF INSERT trigger.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "copy errors in the data",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE e (pk INT PRIMARY KEY, c TEXT NOT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT 'abc', 'x') TO '{TEMPDIR}/doltgrust-kept-bad.txt';",
                    expected: Expected::Tag("COPY 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY e FROM '{TEMPDIR}/doltgrust-kept-bad.txt';",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "abc""#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT 1, 'x', 'y') TO '{TEMPDIR}/doltgrust-kept-bad.txt';",
                    expected: Expected::Tag("COPY 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY e FROM '{TEMPDIR}/doltgrust-kept-bad.txt';",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "extra data after last expected column", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT 1) TO '{TEMPDIR}/doltgrust-kept-bad.txt';",
                    expected: Expected::Tag("COPY 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY e FROM '{TEMPDIR}/doltgrust-kept-bad.txt';",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: r#"missing data for column "c""#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT 1, NULL) TO '{TEMPDIR}/doltgrust-kept-bad.txt';",
                    expected: Expected::Tag("COPY 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY e FROM '{TEMPDIR}/doltgrust-kept-bad.txt';",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"null value in column "c" of relation "e" violates not-null constraint"#, detail: "Failing row contains (1, null).", schema: "public", table: "e", column: "c", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT 1, 'x' UNION ALL SELECT 1, 'y') TO '{TEMPDIR}/doltgrust-kept-bad.txt';",
                    expected: Expected::Tag("COPY 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY e FROM '{TEMPDIR}/doltgrust-kept-bad.txt';",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "e_pkey""#, detail: "Key (pk)=(1) already exists.", schema: "public", table: "e", constraint: "e_pkey", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM e;",
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
                    query: "COPY e FROM '/tmp/doltgrust-kept-missing.txt';",
                    expected: Expected::Error(Diagnostic { code: "58P01", message: r#"could not open file "/tmp/doltgrust-kept-missing.txt" for reading: No such file or directory"#, hint: r#"COPY FROM instructs the PostgreSQL server process to read a file. You may want a client-side facility such as psql's \copy."#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_copy_from_on_error() {
    run_scripts(&[
        ScriptTest {
            name: "COPY FROM's ON_ERROR, LOG_VERBOSITY, and REJECT_LIMIT, from PG18's copy2 regress module",
            set_up_script: &[
                "CREATE TABLE check_ign_err (n int, m int[], k int);",
                "CREATE DOMAIN dcheck_ign_err2 varchar(15) NOT NULL;",
                "CREATE TABLE check_ign_err2 (n int, m int[], k int, l dcheck_ign_err2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COPY check_ign_err FROM STDIN WITH (on_error stop);",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "a""#, context: Some(r#"COPY check_ign_err, line 2, column n: "a""#), ..E }),
                    copy_from_stdin_file: "copy-on-error-stop.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY check_ign_err FROM STDIN WITH (on_error ignore, log_verbosity verbose);",
                    expected: Expected::Tag("COPY 3"),
                    notices: &[
                        Diagnostic { code: "00000", message: r#"skipping row due to data type incompatibility at line 2 for column "n": "a""#, context: Some("COPY check_ign_err"), ..N },
                        Diagnostic { code: "00000", message: r#"skipping row due to data type incompatibility at line 3 for column "k": "3333333333""#, context: Some("COPY check_ign_err"), ..N },
                        Diagnostic { code: "00000", message: r#"skipping row due to data type incompatibility at line 4 for column "m": "{a, 4}""#, context: Some("COPY check_ign_err"), ..N },
                        Diagnostic { code: "00000", message: r#"skipping row due to data type incompatibility at line 5 for column "n": """#, context: Some("COPY check_ign_err"), ..N },
                        Diagnostic { code: "00000", message: r#"skipping row due to data type incompatibility at line 7 for column "m": "a""#, context: Some("COPY check_ign_err"), ..N },
                        Diagnostic { code: "00000", message: r#"skipping row due to data type incompatibility at line 8 for column "k": "a""#, context: Some("COPY check_ign_err"), ..N },
                        Diagnostic { code: "00000", message: "6 rows were skipped due to data type incompatibility", ..N },
                    ],
                    copy_from_stdin_file: "copy-on-error-ignore.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY check_ign_err2 FROM STDIN WITH (on_error ignore, log_verbosity verbose);",
                    expected: Expected::Tag("COPY 1"),
                    notices: &[
                        Diagnostic { code: "00000", message: r#"skipping row due to data type incompatibility at line 2 for column "l": null input"#, context: Some("COPY check_ign_err2"), ..N },
                        Diagnostic { code: "00000", message: "1 row was skipped due to data type incompatibility", ..N },
                    ],
                    copy_from_stdin_file: "copy-on-error-domain-verbose.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY check_ign_err2 FROM STDIN WITH (on_error ignore, log_verbosity silent);",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "copy-on-error-domain-silent.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY check_ign_err FROM STDIN WITH (on_error ignore);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: r#"missing data for column "k""#, context: Some("COPY check_ign_err, line 1: \"1\t{1}\""), ..E }),
                    copy_from_stdin_file: "copy-on-error-missing.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY check_ign_err FROM STDIN WITH (on_error ignore);",
                    expected: Expected::Error(Diagnostic { code: "22P04", message: "extra data after last expected column", context: Some("COPY check_ign_err, line 1: \"1\t{1}\t3\tabc\""), ..E }),
                    copy_from_stdin_file: "copy-on-error-extra.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY check_ign_err FROM STDIN WITH (on_error ignore, reject_limit 3);",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "skipped more than REJECT_LIMIT (3) rows due to data type incompatibility", context: Some(r#"COPY check_ign_err, line 5, column n: """#), ..E }),
                    copy_from_stdin_file: "copy-on-error-reject.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY check_ign_err FROM STDIN WITH (on_error ignore, reject_limit 4);",
                    expected: Expected::Tag("COPY 2"),
                    notices: &[Diagnostic { code: "00000", message: "4 rows were skipped due to data type incompatibility", ..N }],
                    copy_from_stdin_file: "copy-on-error-reject.txt",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM check_ign_err ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("m", INT4_ARRAY), Column("k", INT4)],
                        rows: &[
                            &[T("1"), T("{1}"), T("1")],
                            &[T("5"), T("{5}"), T("5")],
                            &[T("6"), T("{6}"), T("6")],
                            &[T("8"), T("{8}"), T("8")],
                            &[T("10"), T("{10}"), T("10")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM check_ign_err2 ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT4), Column("m", INT4_ARRAY), Column("k", INT4), Column("l", VARCHAR)],
                        rows: &[
                            &[T("1"), T("{1}"), T("1"), T("'foo'")],
                            &[T("3"), T("{3}"), T("3"), T("'bar'")],
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

#[test]
fn test_copy_freeze_into_a_foreign_table() {
    run_scripts(&[
        ScriptTest {
            name: "COPY FREEZE into a foreign table is refused before any row is read",
            set_up_script: &[
                "CREATE FOREIGN DATA WRAPPER copytest_wrapper;",
                "CREATE SERVER copytest_server FOREIGN DATA WRAPPER copytest_wrapper;",
                "CREATE FOREIGN TABLE copytest_foreign_table (a int) SERVER copytest_server;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY copytest_foreign_table FROM STDIN (FREEZE);",
                    copy_from_stdin_file: "tab-load-with-header.sql",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot perform COPY FREEZE on a foreign table", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_copy_into_a_table_its_transaction_emptied() {
    run_scripts(&[
        ScriptTest {
            name: "a COPY into a table its transaction emptied places its rows in input order at its own command",
            set_up_script: &[
                "CREATE TABLE anchor (x int);",
                "CREATE TABLE t (a int PRIMARY KEY, b text, c int UNIQUE);",
                "CREATE INDEX t_b ON t (b);",
                "INSERT INTO t VALUES (100, 'old', 100);",
            ],
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "TRUNCATE t;", expected: Expected::Tag("TRUNCATE TABLE"), ..A },
                ScriptTestAssertion { query: "INSERT INTO anchor VALUES (1);", expected: Expected::Tag("INSERT 0 1"), ..A },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-rows.txt",
                    expected: Expected::Tag("COPY 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t.ctid, t.cmin::text::int - anchor.cmin::text::int AS cmin, t.cmax::text::int - anchor.cmin::text::int AS cmax, a, b, c FROM t, anchor ORDER BY t.ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", INT4), Column("cmax", INT4), Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("1"), T("3"), T("three"), T("30")],
                            &[T("(0,2)"), T("1"), T("1"), T("1"), T("one"), T("10")],
                            &[T("(0,3)"), T("1"), T("1"), T("2"), T("two"), T("20")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion { query: "UPDATE t SET b = 'uno' WHERE a = 1;", expected: Expected::Tag("UPDATE 1"), ..A },
                ScriptTestAssertion {
                    query: "SELECT t.ctid, t.cmin::text::int - anchor.cmin::text::int AS cmin, t.cmax::text::int - anchor.cmin::text::int AS cmax, a, b, c FROM t, anchor ORDER BY t.ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", INT4), Column("cmax", INT4), Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("1"), T("3"), T("three"), T("30")],
                            &[T("(0,3)"), T("1"), T("1"), T("2"), T("two"), T("20")],
                            &[T("(0,4)"), T("2"), T("2"), T("1"), T("uno"), T("10")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4, 'four', 40) RETURNING ctid, cmin::text::int - (SELECT cmin::text::int FROM anchor) AS cmin;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", INT4)],
                        rows: &[&[T("(0,5)"), T("3")]],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion { query: "SET enable_seqscan = off;", expected: Expected::Tag("SET"), ..A },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE b = 'two';",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE c = 30;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[&[T("3")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion { query: "COMMIT;", expected: Expected::Tag("COMMIT"), ..A },
                ScriptTestAssertion {
                    query: "SELECT ctid, a, b, c FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("(0,4)"), T("1"), T("uno"), T("10")],
                            &[T("(0,3)"), T("2"), T("two"), T("20")],
                            &[T("(0,1)"), T("3"), T("three"), T("30")],
                            &[T("(0,5)"), T("4"), T("four"), T("40")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a COPY into a table its transaction emptied fails at the row inserting its rows one at a time fails at",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b text, c int UNIQUE);",
                "CREATE TABLE k (a int, b int UNIQUE);",
                "CREATE TABLE u (a int, b int UNIQUE);",
                "ALTER TABLE u ADD PRIMARY KEY (a);",
            ],
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "TRUNCATE t;", expected: Expected::Tag("TRUNCATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-repeats.txt",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t_c_key""#, detail: "Key (c)=(1) already exists.", context: Some("COPY t, line 3"), schema: "public", table: "t", constraint: "t_c_key", ..E }),
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "TRUNCATE t;", expected: Expected::Tag("TRUNCATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-repeated-key.txt",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t_pkey""#, detail: "Key (a)=(8) already exists.", context: Some("COPY t, line 4"), schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "TRUNCATE k;", expected: Expected::Tag("TRUNCATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "COPY k FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-same-rows.txt",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "k_b_key""#, detail: "Key (b)=(1) already exists.", context: Some("COPY k, line 2"), schema: "public", table: "k", constraint: "k_b_key", ..E }),
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "TRUNCATE u;", expected: Expected::Tag("TRUNCATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "COPY u FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-same-rows.txt",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "u_b_key""#, detail: "Key (b)=(1) already exists.", context: Some("COPY u, line 2"), schema: "public", table: "u", constraint: "u_b_key", ..E }),
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "TRUNCATE k;", expected: Expected::Tag("TRUNCATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "COPY k FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-pairs.txt",
                    expected: Expected::Tag("COPY 2"),
                    ..A
                },
                ScriptTestAssertion { query: "DELETE FROM k WHERE b = 2;", expected: Expected::Tag("DELETE 1"), ..A },
                ScriptTestAssertion { query: "COMMIT;", expected: Expected::Tag("COMMIT"), ..A },
                ScriptTestAssertion {
                    query: "SELECT ctid, a, b FROM k;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", INT4)],
                        rows: &[&[T("(0,1)"), T("1"), T("1")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a COPY canceled into a table its transaction emptied leaves nothing behind",
            set_up_script: &[
                "CREATE FUNCTION cancel_at(n int) RETURNS bool LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN IF n = 3 THEN PERFORM pg_cancel_backend(pg_backend_pid()); PERFORM pg_sleep(0.1); END IF; RETURN true; END $$;",
                "CREATE TABLE t (a int PRIMARY KEY CHECK (cancel_at(a)), b text UNIQUE);",
                "INSERT INTO t VALUES (10, 'ten');",
            ],
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "TRUNCATE t;", expected: Expected::Tag("TRUNCATE TABLE"), ..A },
                ScriptTestAssertion { query: "SAVEPOINT s;", expected: Expected::Tag("SAVEPOINT"), ..A },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-canceled.txt",
                    expected: Expected::Error(Diagnostic {
                        code: "57014",
                        message: "canceling statement due to user request",
                        context: Some("SQL statement \"SELECT pg_cancel_backend(pg_backend_pid())\"\nPL/pgSQL function cancel_at(integer) line 1 at PERFORM\nCOPY t, line 3: \"3\tthree\""),
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK TO s;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-after-cancel.txt",
                    expected: Expected::Tag("COPY 2"),
                    ..A
                },
                ScriptTestAssertion { query: "COMMIT;", expected: Expected::Tag("COMMIT"), ..A },
                ScriptTestAssertion {
                    query: "SELECT ctid, a, b FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("(0,2)"), T("2"), T("two")], &[T("(0,1)"), T("5"), T("five")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion { query: "SET enable_seqscan = off;", expected: Expected::Tag("SET"), ..A },
                ScriptTestAssertion {
                    query: "SELECT a FROM t WHERE b = 'two';",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a COPY into a table its transaction emptied keeps the rows it placed before and what a cursor read before",
            set_up_script: &[
                "CREATE TABLE t (a int PRIMARY KEY, b text);",
                "INSERT INTO t VALUES (10, 'ten');",
            ],
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "TRUNCATE t;", expected: Expected::Tag("TRUNCATE TABLE"), ..A },
                ScriptTestAssertion { query: "INSERT INTO t VALUES (5, 'five'), (1, 'one');", expected: Expected::Tag("INSERT 0 2"), ..A },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-after-rows.txt",
                    expected: Expected::Tag("COPY 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, cmin::text::int - (SELECT min(cmin::text::int) FROM t) AS cmin, a, b FROM t ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("(0,1)"), T("0"), T("5"), T("five")],
                            &[T("(0,2)"), T("0"), T("1"), T("one")],
                            &[T("(0,3)"), T("1"), T("3"), T("three")],
                            &[T("(0,4)"), T("1"), T("2"), T("two")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion { query: "COMMIT;", expected: Expected::Tag("COMMIT"), ..A },
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "TRUNCATE t;", expected: Expected::Tag("TRUNCATE TABLE"), ..A },
                ScriptTestAssertion { query: "DECLARE c CURSOR FOR SELECT a FROM t;", expected: Expected::Tag("DECLARE CURSOR"), ..A },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-one.txt",
                    expected: Expected::Tag("COPY 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL c;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[], tag: "FETCH 0" },
                    ..A
                },
                ScriptTestAssertion { query: "CLOSE c;", expected: Expected::Tag("CLOSE CURSOR"), ..A },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-two.txt",
                    expected: Expected::Tag("COPY 1"),
                    ..A
                },
                ScriptTestAssertion { query: "DECLARE d CURSOR FOR SELECT ctid, cmin, cmax, a FROM t WHERE a = 2;", expected: Expected::Tag("DECLARE CURSOR"), ..A },
                ScriptTestAssertion { query: "UPDATE t SET b = 'deux' WHERE a = 2;", expected: Expected::Tag("UPDATE 1"), ..A },
                ScriptTestAssertion {
                    query: "FETCH ALL d;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("cmax", CID), Column("a", INT4)],
                        rows: &[&[T("(0,2)"), T("0"), T("0"), T("2")]],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion { query: "COMMIT;", expected: Expected::Tag("COMMIT"), ..A },
                ScriptTestAssertion {
                    query: "SELECT ctid, a, b FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", TEXT)],
                        rows: &[&[T("(0,1)"), T("1"), T("one")], &[T("(0,3)"), T("2"), T("deux")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a COPY into a table its transaction emptied that fails leaves its batches up to the failing row placed",
            set_up_script: &["CREATE TABLE t (a int PRIMARY KEY, b text);"],
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "TRUNCATE t;", expected: Expected::Tag("TRUNCATE TABLE"), ..A },
                ScriptTestAssertion { query: "SAVEPOINT s;", expected: Expected::Tag("SAVEPOINT"), ..A },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-repeat-in-batch.txt",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t_pkey""#, detail: "Key (a)=(1) already exists.", context: Some("COPY t, line 3"), schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK TO s;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (9, 'nine') RETURNING ctid;",
                    expected: Expected::Rows { columns: &[Column("ctid", TID)], rows: &[&[T("(0,5)")]], tag: "INSERT 0 1" },
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "TRUNCATE t;", expected: Expected::Tag("TRUNCATE TABLE"), ..A },
                ScriptTestAssertion { query: "SAVEPOINT s;", expected: Expected::Tag("SAVEPOINT"), ..A },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-batches.txt",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t_pkey""#, detail: "Key (a)=(1) already exists.", context: Some("COPY t, line 500"), schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK TO s;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (9999, 'x') RETURNING ctid;",
                    expected: Expected::Rows { columns: &[Column("ctid", TID)], rows: &[&[T("(4,97)")]], tag: "INSERT 0 1" },
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
            ],
            ..S
        },
        ScriptTest {
            name: "a COPY into a table without a primary key its transaction made fails at the later row of a repeated expression or exclusion key",
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "CREATE TABLE cx (a INT, b TEXT);", expected: Expected::Tag("CREATE TABLE"), ..A },
                ScriptTestAssertion { query: "CREATE UNIQUE INDEX ON cx (lower(b));", expected: Expected::Tag("CREATE INDEX"), ..A },
                ScriptTestAssertion {
                    query: "COPY cx FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-cased.txt",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "cx_lower_idx""#, detail: "Key (lower(b))=(x) already exists.", context: Some("COPY cx, line 3"), schema: "public", table: "cx", constraint: "cx_lower_idx", ..E }),
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "CREATE TABLE ce (a INT, b INT, EXCLUDE USING btree (a WITH =));", expected: Expected::Tag("CREATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "COPY ce FROM STDIN;",
                    copy_from_stdin_file: "bulk-copy-same-rows.txt",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: r#"conflicting key value violates exclusion constraint "ce_a_excl""#, detail: "Key (a)=(1) conflicts with existing key (a)=(1).", context: Some("COPY ce, line 2"), schema: "public", table: "ce", constraint: "ce_a_excl", ..E }),
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
            ],
            ..S
        },
    ]);
}
