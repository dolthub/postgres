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

//! Dolt's integration-tests/bats/sql-create-database.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_create_database() {
    run_scripts(&[
        ScriptTest {
            name: "sql-create-database: create new database",
            intent: "A new database takes tables and rows, and is listed with the others.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE mydb;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "m",
                    database: "mydb",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (222);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "m",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("mydb")],
                            &[T("postgres")],
                            &[T("template0")],
                            &[T("template1")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's SHOW DATABASES also lists information_schema, which is a schema in every Postgres database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM test WHERE pk = 222;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "m",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-database: drop database",
            intent: "Dropping a database with commits removes it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE mydb;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (a int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "m",
                    database: "mydb",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "m",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-am', 'first commit')) > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "m",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("first commit")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "m",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "m",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE mydb;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM pg_database WHERE datname = 'mydb';",
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
            name: "sql-create-database: drop and recreate database",
            intent: "A dropped database's name can be used again.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE mydb;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE mydb;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE mydb;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM pg_database WHERE datname = 'mydb';",
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
            name: "sql-create-database: drop current database",
            intent: "The database a session is connected to cannot be dropped.",
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP DATABASE postgres;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: "cannot drop the currently open database", ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses to drop the database a session is connected to; Dolt skips this test (\"unsupported to drop the current DB, but need to have a way\")")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-database: create database that already exists throws an error",
            intent: "Creating a database that exists fails.",
            set_up_script: &[
                "CREATE DATABASE mydb;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE mydb;",
                    expected: Expected::Error(Diagnostic { code: "42P04", message: "database \"mydb\" already exists", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-database: create database IF NOT EXISTS on database that already exists doesn't throw an error",
            intent: "CREATE DATABASE IF NOT EXISTS skips a database that exists.",
            set_up_script: &[
                "CREATE DATABASE mydb;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE IF NOT EXISTS mydb;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"NOT\"", position: 20, ..E }),
                    skip: Some("CREATE DATABASE IF NOT EXISTS stays as Doltgrust-only syntax, which skips a database that exists (the owner, 2026-10-26, Appendix C)"),
                    dolt: Some(Dolt::MySqlOnly("CREATE DATABASE IF NOT EXISTS is MySQL's")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-database: create and drop new database in same session",
            intent: "A database dropped in the session that created it is gone.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE mydb;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE mydb;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::ClientError("FATAL: database \"mydb\" does not exist (SQLSTATE 3D000)"),
                    client: "m",
                    database: "mydb",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-database: create new database IF NOT EXISTS",
            intent: "CREATE DATABASE IF NOT EXISTS creates a missing database, and DROP DATABASE IF EXISTS skips a missing one.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE IF EXISTS test;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"EXISTS\"", position: 20, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE IF NOT EXISTS test;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"NOT\"", position: 20, ..E }),
                    skip: Some("CREATE DATABASE IF NOT EXISTS stays as Doltgrust-only syntax, which skips a database that exists (the owner, 2026-10-26, Appendix C)"),
                    dolt: Some(Dolt::MySqlOnly("CREATE DATABASE IF NOT EXISTS is MySQL's")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE test;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                            &[T("template0")],
                            &[T("template1")],
                            &[T("test")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "t",
                    database: "test",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (222);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "t",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM test WHERE pk = 222;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "t",
                    ..A
                },
                ScriptTestAssertion {
                    query: "",
                    expected: Expected::Ok,
                    client: "t",
                    close_client: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE test;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE IF NOT EXISTS test;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"NOT\"", position: 20, ..E }),
                    skip: Some("CREATE DATABASE IF NOT EXISTS stays as Doltgrust-only syntax, which skips a database that exists (the owner, 2026-10-26, Appendix C)"),
                    dolt: Some(Dolt::MySqlOnly("CREATE DATABASE IF NOT EXISTS is MySQL's")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE test;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE IF EXISTS test;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE IF EXISTS test;",
                    expected: Expected::Tag("DROP DATABASE"),
                    notices: &[Diagnostic { code: "00000", message: "database \"test\" does not exist, skipping", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE IF NOT EXISTS test;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"NOT\"", position: 18, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-database: drop database throws error for database that doesn't exist",
            intent: "Dropping a missing database fails.",
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP DATABASE mydb;",
                    expected: Expected::Error(Diagnostic { code: "3D000", message: "database \"mydb\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-database: use for non existing database throws an error",
            intent: "Connecting to a missing database fails.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::ClientError("FATAL: database \"test\" does not exist (SQLSTATE 3D000)"),
                    client: "t",
                    database: "test",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-database: SHOW DATABASES works after CREATE and DROP",
            intent: "Creating and dropping a database leaves the list of databases as it was.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                            &[T("template0")],
                            &[T("template1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE hi;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DATABASE hi;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                            &[T("template0")],
                            &[T("template1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-database: create database with character set should parse correctly and not error",
            intent: "A database created with an encoding and collation records them.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE metabase ENCODING 'UTF8' LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci becomes Postgres' ENCODING, LC_COLLATE, and LC_CTYPE, and information_schema.SCHEMATA becomes pg_database")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname, pg_encoding_to_char(encoding), datcollate, datctype FROM pg_database WHERE datname = 'metabase';",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("pg_encoding_to_char", NAME), Column("datcollate", TEXT), Column("datctype", TEXT)],
                        rows: &[
                            &[T("metabase"), T("UTF8"), T("C"), T("C")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-database: creating database with hyphen and space characters allowed",
            intent: "A database's name may hold hyphens and leading and trailing spaces.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE \" test- db _  \";",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_database() AS a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", NAME)],
                        rows: &[
                            &[T(" test- db _  ")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "d",
                    database: " test- db _  ",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
