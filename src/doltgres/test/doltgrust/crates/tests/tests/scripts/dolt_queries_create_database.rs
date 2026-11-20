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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_create_database.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_create_database_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "create database simple",
            intent: "A database made by CREATE DATABASE is listed and takes tables and rows, and a session on another database can't write it through a qualified name.",
            set_up_script: &[
                "CREATE DATABASE test;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE NOT datistemplate ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("postgres")],
                            &[T("test")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES is MySQL's and lists information_schema and mysql as databases, while pg_database lists databases only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE foo (bar INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "test",
                    database: "test",
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE switches a session's database, and a Postgres connection's database never changes, so the port connects to test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test.public.foo VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cross-database references are not implemented: \"test.public.foo\"", position: 13, ..E }),
                    dolt: Some(Dolt::Differs("Dolt writes another database through a qualified name, and Doltgrust writes another database only through postgres_fdw, Appendix C \"cross-database statements\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO foo VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "test",
                    database: "test",
                    dolt: Some(Dolt::Differs("the row goes in through a connection to test, since the qualified write above is refused")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test.public.foo;",
                    expected: Expected::Rows {
                        columns: &[Column("bar", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("reading another database without switching (STORAGE_DESIGN §V.3)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM foo;",
                    expected: Expected::Rows {
                        columns: &[Column("bar", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test",
                    database: "test",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create database with non standard collation, create branch",
            intent: "A branch made in a database created with a collation other than the default keeps the database's collation.",
            set_up_script: &[
                "CREATE DATABASE test TEMPLATE template0 ENCODING 'UTF8' LOCALE_PROVIDER icu ICU_LOCALE 'und-u-ks-level2';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('b1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test",
                    database: "test",
                    dolt: Some(Dolt::MySqlOnly("MySQL's USE switches a session's database, and a Postgres connection's database never changes, so the port connects to test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datlocprovider, datlocale FROM pg_database WHERE datname = 'test';",
                    expected: Expected::Rows {
                        columns: &[Column("datlocprovider", CHAR), Column("datlocale", TEXT)],
                        rows: &[
                            &[T("i"), T("und-u-ks-level2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test",
                    database: "test",
                    dolt: Some(Dolt::MySqlOnly("MySQL's utf8mb4_unicode_ci is ICU's case-insensitive root collation in Postgres, and SHOW CREATE DATABASE is MySQL's, so the port reads pg_database")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create database in a transaction",
            intent: "CREATE DATABASE inside an open transaction makes a database a session can use at once.",
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE test;",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "CREATE DATABASE cannot run inside a transaction block", ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses CREATE DATABASE inside a transaction block, so the rest of Dolt's script, which uses the new database, has nothing to run on")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_database WHERE datname = 'test';",
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
            name: "create database and schema forbid revision delimiters in names",
            intent: "CREATE DATABASE refuses a name holding @ or /, which would read as a revision of a database, inside a transaction too, while a plain name works there.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE \"postgres@branch\";",
                    expected: Expected::Error(Diagnostic { code: "42602", message: "unacceptable database name \"postgres@branch\"", detail: "The characters \"/\" and \"@\" mark a revision of a database.", ..E }),
                    dolt: Some(Dolt::Differs("Postgres accepts the name, and Doltgrust refuses it as Dolt does, with Postgres' code for an invalid name, Appendix C \"revision connections\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE \"postgres/too\";",
                    expected: Expected::Error(Diagnostic { code: "42602", message: "unacceptable database name \"postgres/too\"", detail: "The characters \"/\" and \"@\" mark a revision of a database.", ..E }),
                    dolt: Some(Dolt::Differs("Postgres accepts the name, and Doltgrust refuses it as Dolt does, with Postgres' code for an invalid name, Appendix C \"revision connections\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA \"postgres@branch\";",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    dolt: Some(Dolt::MySqlOnly("a MySQL schema is a database, and a Postgres schema never names a revision, so its name may hold @ and /")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA \"postgres/too\";",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    dolt: Some(Dolt::MySqlOnly("a MySQL schema is a database, and a Postgres schema never names a revision, so its name may hold @ and /")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE IF NOT EXISTS \"postgres@branch\";",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"NOT\"", position: 20, ..E }),
                    dolt: Some(Dolt::MySqlOnly("CREATE DATABASE IF NOT EXISTS is MySQL's")),
                    skip: Some("CREATE DATABASE IF NOT EXISTS stays as Doltgrust-only syntax, which skips a database that exists (the owner, 2026-10-26, Appendix C)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA IF NOT EXISTS \"postgres/too\";",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    notices: &[Diagnostic { code: "42P06", message: "schema \"postgres/too\" already exists, skipping", ..N }],
                    dolt: Some(Dolt::MySqlOnly("a MySQL schema is a database, and a Postgres schema never names a revision, so its name may hold @ and /")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE \"postgres@txbad\";",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "CREATE DATABASE cannot run inside a transaction block", ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses CREATE DATABASE inside a transaction block before it reads the name")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE tx_ok;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    dolt: Some(Dolt::Differs("Postgres refuses CREATE DATABASE inside a transaction block, so the port makes it after the block")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE datname LIKE 'tx_ok';",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("tx_ok")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW DATABASES LIKE is MySQL's, and pg_database lists databases")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
