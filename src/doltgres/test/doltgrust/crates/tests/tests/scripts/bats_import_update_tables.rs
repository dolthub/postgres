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

//! Dolt's integration-tests/bats/import-update-tables.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts};

#[test]
fn test_import_update_tables() {
    run_scripts(&[
        ScriptTest {
            name: "import-update-tables: update table using csv",
            intent: "An upsert adds new rows, changes nothing when repeated, and updates a changed row.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c1, c2, c3, c4, c5) VALUES (0,1,2,3,4,5), (1,1,2,3,4,5) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2, c3 = EXCLUDED.c3, c4 = EXCLUDED.c4, c5 = EXCLUDED.c5;",
                    expected: Expected::Tag("INSERT 0 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c1, c2, c3, c4, c5) VALUES (0,1,2,3,4,5), (1,1,2,3,4,5) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2, c3 = EXCLUDED.c3, c4 = EXCLUDED.c4, c5 = EXCLUDED.c5;",
                    expected: Expected::Tag("INSERT 0 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c1, c2, c3, c4, c5) VALUES (0,1,2,3,4,6) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2, c3 = EXCLUDED.c3, c4 = EXCLUDED.c4, c5 = EXCLUDED.c5;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("6")],
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
            name: "import-update-tables: update table using csv with null (blank) values",
            intent: "A CSV file whose fields don't fit the integer columns fails to load.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type bigint: \"1,2,3,4,5,\n1,1,,,2,3\"", ..E }),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"nulls from csv not working correctly on update\"), and its 1pk5col-nulls.csv holds newlines inside the integer fields, which no integer column takes")),
                    copy_from_stdin_file: "bats-import-update-nulls.csv",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: update table using csv with newlines",
            intent: "Quoted CSV fields keep their newlines.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk varchar(20) NOT NULL, c1 text, c2 text, c3 text, c4 text, c5 text, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY test FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN loads the file into the empty table")),
                    copy_from_stdin_file: "bats-import-update-newlines.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("c1", TEXT), Column("c2", TEXT), Column("c3", TEXT), Column("c4", TEXT), Column("c5", TEXT)],
                        rows: &[
                            &[T("0"), T("1\n1"), T("2"), T("3"), T("4"), T("5")],
                            &[T("1"), T("1"), T("2"), T("3\n3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: updating table by inputting string longer than char column throws an error",
            intent: "A value longer than a char column fails.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c CHAR(5), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c) VALUES (1, '123456') ON CONFLICT (pk) DO UPDATE SET c = EXCLUDED.c;",
                    expected: Expected::Error(Diagnostic { code: "22001", message: "value too long for type character(5)", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: update table with repeat pk in csv does not throw an error",
            intent: "Rows repeating one key in a single upsert are refused.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c1, c2, c3, c4, c5) VALUES (1,1,2,3,4,5), (1,1,2,3,4,5) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2, c3 = EXCLUDED.c3, c4 = EXCLUDED.c4, c5 = EXCLUDED.c5;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "ON CONFLICT DO UPDATE command cannot affect row a second time", hint: "Ensure that no rows proposed for insertion within the same command have duplicate constrained values.", ..E }),
                    dolt: Some(Dolt::Differs("Postgres' ON CONFLICT refuses a statement that reaches one row twice, where dolt table import applies the file's rows in turn")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
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
            name: "import-update-tables: importing into new table renders bad rows",
            intent: "Rows failing a check constraint make the load fail and write nothing.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE persons (ID int PRIMARY KEY, LastName varchar(255) NOT NULL, FirstName varchar(255), Age int CHECK (Age>=18));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO persons (ID, LastName, FirstName, Age) VALUES (1,'jon','doe',20), (2,'little','doe',10), (3,'little','doe',4), (4,'little','doe',1) ON CONFLICT (ID) DO UPDATE SET LastName = EXCLUDED.LastName, FirstName = EXCLUDED.FirstName, Age = EXCLUDED.Age;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"persons\" violates check constraint \"persons_age_check\"", detail: "Failing row contains (2, little, doe, 10).", schema: "public", table: "persons", constraint: "persons_age_check", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names, and its --continue option, which skips the bad rows, is the CLI's and left out")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM persons;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("lastname", VARCHAR), Column("firstname", VARCHAR), Column("age", INT4)],
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
            name: "import-update-tables: importing some columns does not overwrite columns not part of the import",
            intent: "Upserts naming some columns leave the other columns as they were.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c1) VALUES (1,1), (2,6) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1;",
                    expected: Expected::Tag("INSERT 0 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c2) VALUES (1,2), (2,7) ON CONFLICT (pk) DO UPDATE SET c2 = EXCLUDED.c2;",
                    expected: Expected::Tag("INSERT 0 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c3, c4, c5) VALUES (1,3,4,5), (2,8,9,10) ON CONFLICT (pk) DO UPDATE SET c3 = EXCLUDED.c3, c4 = EXCLUDED.c4, c5 = EXCLUDED.c5;",
                    expected: Expected::Tag("INSERT 0 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("2"), T("3"), T("4"), T("5")],
                            &[T("2"), T("6"), T("7"), T("8"), T("9"), T("10")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: error during primary key table just skips",
            intent: "A key failing a check constraint makes the load fail and write nothing.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk int PRIMARY KEY CHECK (pk < 10));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (1), (2), (100), (3) ON CONFLICT (pk) DO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"test\" violates check constraint \"test_pk_check\"", detail: "Failing row contains (100).", schema: "public", table: "test", constraint: "test_pk_check", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT writes the file's rows, and its --continue option, which skips the bad rows, is the CLI's and left out")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
            name: "import-update-tables: string too large for column regression",
            intent: "A row with short strings, a date, and integers loads into a table with a three-column key.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (STATE_ID smallint NOT NULL, NIBRS_MONTH_ID bigint NOT NULL, AGENCY_ID bigint NOT NULL, AGENCY_TABLE_TYPE_ID smallint NOT NULL, MONTH_NUM smallint NOT NULL, DATA_YEAR smallint NOT NULL, REPORTED_STATUS varchar(1), REPORT_DATE timestamp, UPDATE_FLAG char(1), ORIG_FORMAT char(1), DATA_HOME varchar(1), DDOCNAME varchar(50), DID bigint, MONTH_PUB_STATUS int, INC_DATA_YEAR int, PRIMARY KEY (STATE_ID,NIBRS_MONTH_ID,DATA_YEAR));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's column collations and secondary KEY clauses have no bearing on the import, so the table leaves them out")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (INC_DATA_YEAR, NIBRS_MONTH_ID, AGENCY_ID, MONTH_NUM, DATA_YEAR, REPORTED_STATUS, REPORT_DATE, UPDATE_FLAG, ORIG_FORMAT, DATA_HOME, DDOCNAME, DID, MONTH_PUB_STATUS, STATE_ID, AGENCY_TABLE_TYPE_ID) VALUES (2019,9128595,9305,3,2019,'I','2019-07-18','Y','F','C','2019_03_MN0510000_NIBRS',49502383,0,27,2) ON CONFLICT (STATE_ID, NIBRS_MONTH_ID, DATA_YEAR) DO UPDATE SET INC_DATA_YEAR = EXCLUDED.INC_DATA_YEAR, AGENCY_ID = EXCLUDED.AGENCY_ID, MONTH_NUM = EXCLUDED.MONTH_NUM, REPORTED_STATUS = EXCLUDED.REPORTED_STATUS, REPORT_DATE = EXCLUDED.REPORT_DATE, UPDATE_FLAG = EXCLUDED.UPDATE_FLAG, ORIG_FORMAT = EXCLUDED.ORIG_FORMAT, DATA_HOME = EXCLUDED.DATA_HOME, DDOCNAME = EXCLUDED.DDOCNAME, DID = EXCLUDED.DID, MONTH_PUB_STATUS = EXCLUDED.MONTH_PUB_STATUS, AGENCY_TABLE_TYPE_ID = EXCLUDED.AGENCY_TABLE_TYPE_ID;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: csv subsetting throws error with not null column",
            intent: "A load that leaves out a NOT NULL column without a default fails.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk int NOT NULL, c1 int, c2 int, c3 int, c4 int NOT NULL, c5 int, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c1, c2, c5, c3) VALUES (0,1,2,6,3) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2, c5 = EXCLUDED.c5, c3 = EXCLUDED.c3;",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"c4\" of relation \"test\" violates not-null constraint", detail: "Failing row contains (0, 1, 2, 3, null, 6).", schema: "public", table: "test", column: "c4", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: csv subsetting but with defaults",
            intent: "A load that leaves out a column fills it with the column's default.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk int NOT NULL, c1 int, c2 int, c3 int, c4 int DEFAULT 42, c5 int, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c1, c2, c5, c3) VALUES (0,1,2,6,3) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2, c5 = EXCLUDED.c5, c3 = EXCLUDED.c3;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4), Column("c3", INT4), Column("c4", INT4), Column("c5", INT4)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("42"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: csv file has less columns than schema -u",
            intent: "A load that leaves out a nullable column without a default leaves it NULL.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c1, c2, c5, c3) VALUES (0,1,2,6,3) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2, c5 = EXCLUDED.c5, c3 = EXCLUDED.c3;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), Null, T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: csv files has same number of columns but different order than schema",
            intent: "Columns named in another order load into the right columns.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c2, c4, c5, c1, c3) VALUES (0,2,4,6,1,3) ON CONFLICT (pk) DO UPDATE SET c2 = EXCLUDED.c2, c4 = EXCLUDED.c4, c5 = EXCLUDED.c5, c1 = EXCLUDED.c1, c3 = EXCLUDED.c3;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("2"), T("3"), T("4"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: just update one column in a big table",
            intent: "An upsert of one column changes only that column.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (0,1,2,3,4,5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, c2) VALUES (0,7) ON CONFLICT (pk) DO UPDATE SET c2 = EXCLUDED.c2;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("c1", INT8), Column("c2", INT8), Column("c3", INT8), Column("c4", INT8), Column("c5", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("7"), T("3"), T("4"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: updating a table with no primary keys complains",
            intent: "A load without the primary key column fails.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (c2) VALUES (70);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"pk\" of relation \"test\" violates not-null constraint", detail: "Failing row contains (null, null, 70, null, null, null).", schema: "public", table: "test", column: "pk", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT writes the file's rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: partial update on keyless table",
            intent: "Loading into a table without a primary key adds a row, filling a left-out column with its default.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE keyless (c0 int, c1 int DEFAULT 42, c2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (0,1,0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless (c0, c2) VALUES (0, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT writes the file's rows, which add rows to a table without a primary key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless ORDER BY c0, c1 DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("42"), T("2")],
                            &[T("0"), T("1"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: import supports tables with dashes in the name",
            intent: "A table whose name has dashes takes inserts and upserts.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE \"this-is-a-table\" (pk int PRIMARY KEY, c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -c is the CLI's, so CREATE TABLE makes the table with the types it infers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"this-is-a-table\" (pk, c1) VALUES (0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT writes the file's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"this-is-a-table\" (pk, c1) VALUES (0, 0) ON CONFLICT (pk) DO UPDATE SET c1 = EXCLUDED.c1;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"this-is-a-table\";",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: successfully update child table in fk relationship",
            intent: "Rows whose foreign key values exist in the parent load.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id, name, color) VALUES (4,'laptop','blue'), (5,'dollar','green'), (6,'bottle','red') ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name, color = EXCLUDED.color;",
                    expected: Expected::Tag("INSERT 0 3"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM objects WHERE id >= 4 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR), Column("color", VARCHAR)],
                        rows: &[
                            &[T("4"), T("laptop"), T("blue")],
                            &[T("5"), T("dollar"), T("green")],
                            &[T("6"), T("bottle"), T("red")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: unsuccessfully update child table in fk relationship",
            intent: "A row whose foreign key value is missing from the parent makes the load fail and write nothing.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id, name, color) VALUES (4,'laptop','blue'), (5,'dollar','green'), (6,'bottle','gray') ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name, color = EXCLUDED.color;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"objects\" violates foreign key constraint \"objects_color_fkey\"", detail: "Key (color)=(gray) is not present in table \"colors\".", schema: "public", table: "objects", constraint: "objects_color_fkey", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names, and its --continue option, which skips the bad rows, is the CLI's and left out")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM objects WHERE id >= 4 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR), Column("color", VARCHAR)],
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
            name: "import-update-tables: successfully update child table in multi-key fk relationship",
            intent: "Rows matching a two-column foreign key load, and a row matching no parent fails.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE objects;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE colors;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX color_index ON colors (color);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the referenced index is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE materials (id int NOT NULL, material VARCHAR(32) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX color_mat_index ON materials (color, material);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the referenced index is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), material VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color,material) REFERENCES materials(color,material));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple'),(10,'brown');",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO materials (id,material,color) VALUES (1,'steel','red'),(2,'rubber','green'),(3,'leather','blue'),(10,'dirt','brown'),(11,'air',NULL);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id,name,color,material) VALUES (1,'truck','red','steel'),(2,'ball','green','rubber'),(3,'shoe','blue','leather'),(11,'tornado',NULL,'air');",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id, name, color, material) VALUES (4,'laptop','red','steel'), (5,'dollar','green','rubber'), (6,'bottle','blue','leather') ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name, color = EXCLUDED.color, material = EXCLUDED.material;",
                    expected: Expected::Tag("INSERT 0 3"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM objects WHERE id >= 4 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR), Column("color", VARCHAR), Column("material", VARCHAR)],
                        rows: &[
                            &[T("4"), T("laptop"), T("red"), T("steel")],
                            &[T("5"), T("dollar"), T("green"), T("rubber")],
                            &[T("6"), T("bottle"), T("blue"), T("leather")],
                            &[T("11"), T("tornado"), Null, T("air")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id, name, color, material) VALUES (4,'laptop','red','steel'), (5,'dollar','green','rubber'), (6,'bottle','blue','steel') ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name, color = EXCLUDED.color, material = EXCLUDED.material;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"objects\" violates foreign key constraint \"objects_color_material_fkey\"", detail: "Key (color, material)=(blue, steel) is not present in table \"materials\".", schema: "public", table: "objects", constraint: "objects_color_material_fkey", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names, and its --continue option, which skips the bad rows, is the CLI's and left out")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM objects WHERE id >= 4 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR), Column("color", VARCHAR), Column("material", VARCHAR)],
                        rows: &[
                            &[T("4"), T("laptop"), T("red"), T("steel")],
                            &[T("5"), T("dollar"), T("green"), T("rubber")],
                            &[T("6"), T("bottle"), T("blue"), T("leather")],
                            &[T("11"), T("tornado"), Null, T("air")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: unsuccessfully update parent table in fk relationship",
            intent: "Changing a referenced parent value fails and writes nothing.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO colors (id, color) VALUES (3,'dsadasda'), (5,'yellow') ON CONFLICT (id) DO UPDATE SET color = EXCLUDED.color;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "update or delete on table \"colors\" violates foreign key constraint \"objects_color_fkey\" on table \"objects\"", detail: "Key (color)=(blue) is still referenced from table \"objects\".", schema: "public", table: "objects", constraint: "objects_color_fkey", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names, and its --continue option, which skips the bad rows, is the CLI's and left out")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM colors WHERE id IN (3,5) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("color", VARCHAR)],
                        rows: &[
                            &[T("3"), T("blue")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: circular foreign keys",
            intent: "A table referencing itself takes a row referencing an existing row and refuses one referencing none.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE tbl (id int PRIMARY KEY, v1 int, v2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX v1 ON tbl (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::Differs("Postgres requires a unique index on the referenced columns (42830), so the referenced index is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX v2 ON tbl (v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tbl ADD CONSTRAINT fk_named FOREIGN KEY (v2) REFERENCES tbl(v1) ON UPDATE CASCADE ON DELETE CASCADE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tbl VALUES (1,1,1), (2, 2, 1), (3, 3, NULL);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tbl (id, v1, v2) VALUES (4,4,2) ON CONFLICT (id) DO UPDATE SET v1 = EXCLUDED.v1, v2 = EXCLUDED.v2;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tbl (id, v1, v2) VALUES (5,5,1), (6,6,1000) ON CONFLICT (id) DO UPDATE SET v1 = EXCLUDED.v1, v2 = EXCLUDED.v2;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"tbl\" violates foreign key constraint \"fk_named\"", detail: "Key (v2)=(1000) is not present in table \"tbl\".", schema: "public", table: "tbl", constraint: "fk_named", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: disable foreign key checks",
            intent: "With foreign key checks off a row with a missing parent loads, and verifying the constraints finds the violation.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET session_replication_role = replica;",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import's --disable-fk-checks is the CLI's, so session_replication_role = replica turns off the foreign key triggers")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO objects (id, name, color) VALUES (4,'laptop','blue'), (5,'dollar','green'), (6,'bottle','gray') ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name, color = EXCLUDED.color;",
                    expected: Expected::Tag("INSERT 0 3"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM objects WHERE id = 6;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR), Column("color", VARCHAR)],
                        rows: &[
                            &[T("6"), T("bottle"), T("gray")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET session_replication_role = DEFAULT;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_verify_constraints('objects');",
                    expected: Expected::Rows {
                        columns: &[Column("violations", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt constraints verify checks the constraints, which dolt_verify_constraints() does")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: enum type",
            intent: "Enum values load, and a value outside the enum fails.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TYPE size AS ENUM ('x-small', 'small', 'medium', 'large', 'x-large');",
                    expected: Expected::Tag("CREATE TYPE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's inline ENUM column type is a type made with CREATE TYPE ... AS ENUM")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int PRIMARY KEY, size size);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (pk, size) VALUES (1,'small'), (2,'medium'), (3,'large') ON CONFLICT (pk) DO UPDATE SET size = EXCLUDED.size;",
                    expected: Expected::Tag("INSERT 0 3"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("size", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("small")],
                            &[T("2"), T("medium")],
                            &[T("3"), T("large")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (pk, size) VALUES (1,'small'), (2,'medium'), (3,'large'), (4,'dasdas') ON CONFLICT (pk) DO UPDATE SET size = EXCLUDED.size;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input value for enum size: \"dasdas\"", position: 76, ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names, and its --continue option is the CLI's and left out; Dolt skips this test (\"dolt is improperly giving a default value for a bad enum value on --continue\")")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("size", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("small")],
                            &[T("2"), T("medium")],
                            &[T("3"), T("large")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: import table with absent auto-increment column",
            intent: "Rows without the identity column get generated keys.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE tbl (id int GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, v1 int, v2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's AUTO_INCREMENT is an identity column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX v1 ON tbl (v1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX v2 ON tbl (v2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tbl (v1, v2) VALUES (4,2), (3,1);",
                    expected: Expected::Tag("INSERT 0 2"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT writes the file's rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tbl ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v1", INT4), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("4"), T("2")],
                            &[T("2"), T("3"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: distinguish between empty string and null for ENUMs",
            intent: "A quoted empty CSV field loads as the empty enum label and an empty field as NULL.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TYPE letter AS ENUM ('', 'a', 'b');",
                    expected: Expected::Tag("CREATE TYPE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's inline ENUM column type is a type made with CREATE TYPE ... AS ENUM")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE alphabet(pk int PRIMARY KEY, letter letter);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY alphabet FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 3"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN loads the file into the empty table")),
                    copy_from_stdin_file: "bats-import-update-enum.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, letter, letter IS NULL FROM alphabet ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("letter", USER_DEFINED), Column("?column?", BOOL)],
                        rows: &[
                            &[T("1"), T("a"), T("f")],
                            &[T("2"), T(""), T("f")],
                            &[T("3"), Null, T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: table has more columns than flat file warns and continues for CSV and Parquet",
            intent: "A load that leaves out a column with a default fills it.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE with_created_at_csv (id int PRIMARY KEY, name VARCHAR(100), created_at timestamp DEFAULT CURRENT_TIMESTAMP);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO with_created_at_csv (id, name) VALUES (1, 'alice');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO with_created_at_csv (id, name) VALUES (2, 'bob') ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so INSERT ... ON CONFLICT DO UPDATE writes the file's rows and updates the columns it names, and the Parquet half of the test is the CLI's and left out")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, name, created_at IS NOT NULL FROM with_created_at_csv ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR), Column("?column?", BOOL)],
                        rows: &[
                            &[T("1"), T("alice"), T("t")],
                            &[T("2"), T("bob"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "import-update-tables: import csv with invalid utf8",
            intent: "A file with a byte that isn't UTF-8 fails to load.",
            set_up_script: &[
                "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id)); CREATE UNIQUE INDEX color_index ON colors (color);",
                "CREATE TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                "INSERT INTO colors (id,color) VALUES (1,'red'),(2,'green'),(3,'blue'),(4,'purple');",
                "INSERT INTO objects (id,name,color) VALUES (1,'truck','red'),(2,'ball','green'),(3,'shoe','blue');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE item (id VARCHAR(10) NOT NULL, name VARCHAR(255), PRIMARY KEY (id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY item FROM STDIN WITH (FORMAT csv, HEADER);",
                    expected: Expected::Error(Diagnostic { code: "22021", message: "invalid byte sequence for encoding \"UTF8\": 0x85", ..E }),
                    dolt: Some(Dolt::MySqlOnly("dolt table import -u is the CLI's, so COPY ... FROM STDIN loads the file into the empty table, and --continue's truncation at the bad byte is MySQL's and left out")),
                    copy_from_stdin_file: "bats-import-update-invalid-utf8.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM item;",
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
