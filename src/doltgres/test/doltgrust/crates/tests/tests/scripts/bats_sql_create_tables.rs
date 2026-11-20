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

//! Dolt's integration-tests/bats/sql-create-tables.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_create_tables() {
    run_scripts(&[
        ScriptTest {
            name: "sql-create-tables: create a single primary key table",
            intent: "CREATE TABLE with a one-column primary key makes a table the database lists.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a two primary key table",
            intent: "CREATE TABLE with a two-column primary key makes a table the database lists.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a table that uses all supported types",
            intent: "A table with integer, text, boolean, float, unsigned, and fixed-length string columns is created with each column's type.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (\"pk\" bigint NOT NULL, \"int\" bigint, \"string\" text, \"boolean\" BOOLEAN, \"float\" double precision, \"uint\" numeric(20), \"uuid\" CHAR(36), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's BIGINT UNSIGNED is numeric(20), and CHAR(36)'s ascii character set and collation have no Postgres form here")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("t"), T("t")],
                            &[T("int"), T("bigint"), T("f"), T("f")],
                            &[T("string"), T("text"), T("f"), T("f")],
                            &[T("boolean"), T("boolean"), T("f"), T("f")],
                            &[T("float"), T("double precision"), T("f"), T("f")],
                            &[T("uint"), T("numeric(20,0)"), T("f"), T("f")],
                            &[T("uuid"), T("character(36)"), T("f"), T("f")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_attribute and pg_index show each column's type, NOT NULL, and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a table that uses unsupported poop type",
            intent: "CREATE TABLE with an unknown column type fails.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (\"pk\" bigint NOT NULL, \"int\" bigint, \"string\" text, \"boolean\" BOOLEAN, \"float\" double precision, \"uint\" numeric(20), \"uuid\" CHAR(36), \"blob\" bytea, \"poop\" POOP, PRIMARY KEY (pk));",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "type \"poop\" does not exist", position: 174, ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's BIGINT UNSIGNED is numeric(20), and CHAR(36)'s ascii character set and collation have no Postgres form here")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a repo with two tables",
            intent: "Two created tables are both listed.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test1 (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk1 bigint NOT NULL, pk2 bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1,pk2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test1")],
                            &[T("test2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a basic table (int types) using sql",
            intent: "A primary key column is NOT NULL even when the statement doesn't say so.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("t"), T("t")],
                            &[T("c1"), T("bigint"), T("f"), T("f")],
                            &[T("c2"), T("bigint"), T("f"), T("f")],
                            &[T("c3"), T("bigint"), T("f"), T("f")],
                            &[T("c4"), T("bigint"), T("f"), T("f")],
                            &[T("c5"), T("bigint"), T("f"), T("f")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_attribute and pg_index show each column's type, NOT NULL, and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a table with sql with multiple primary keys",
            intent: "Every column of a two-column primary key is NOT NULL.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk1 bigint, pk2 bigint, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk1, pk2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk1"), T("bigint"), T("t"), T("t")],
                            &[T("pk2"), T("bigint"), T("t"), T("t")],
                            &[T("c1"), T("bigint"), T("f"), T("f")],
                            &[T("c2"), T("bigint"), T("f"), T("f")],
                            &[T("c3"), T("bigint"), T("f"), T("f")],
                            &[T("c4"), T("bigint"), T("f"), T("f")],
                            &[T("c5"), T("bigint"), T("f"), T("f")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_attribute and pg_index show each column's type, NOT NULL, and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a table using sql with not null constraint",
            intent: "A NOT NULL primary key column and nullable bigint columns keep their types.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("t"), T("t")],
                            &[T("c1"), T("bigint"), T("f"), T("f")],
                            &[T("c2"), T("bigint"), T("f"), T("f")],
                            &[T("c3"), T("bigint"), T("f"), T("f")],
                            &[T("c4"), T("bigint"), T("f"), T("f")],
                            &[T("c5"), T("bigint"), T("f"), T("f")],
                        ],
                        tag: "SELECT 6",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_attribute and pg_index show each column's type, NOT NULL, and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a table using sql with a float",
            intent: "A double precision column keeps its type.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 double precision, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("t"), T("t")],
                            &[T("c1"), T("double precision"), T("f"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_attribute and pg_index show each column's type, NOT NULL, and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a table using sql with a string",
            intent: "A text column keeps its type.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 text, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's LONGTEXT is text")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("t"), T("t")],
                            &[T("c1"), T("text"), T("f"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_attribute and pg_index show each column's type, NOT NULL, and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a table using sql with an unsigned int",
            intent: "An unsigned bigint column is created with a type that holds its range.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 numeric(20), PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's unsigned integer types are the next wider signed type")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("t"), T("t")],
                            &[T("c1"), T("numeric(20,0)"), T("f"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_attribute and pg_index show each column's type, NOT NULL, and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a table using sql with a boolean",
            intent: "A boolean column can be created.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 BOOLEAN, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a table with a mispelled primary key",
            intent: "A primary key naming a missing column fails.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint, c1 bigint, c2 bigint, PRIMARY KEY\n(pk,noexist));",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"noexist\" named in key does not exist", position: 53, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a table with a SQL reserved word",
            intent: "Reserved words are column names only when quoted.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk int NOT NULL, \"all\" int, \"select\" int, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("integer"), T("t"), T("t")],
                            &[T("all"), T("integer"), T("f"), T("f")],
                            &[T("select"), T("integer"), T("f"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_attribute and pg_index show each column's type, NOT NULL, and primary key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk int NOT NULL, all int, select int, PRIMARY KEY (pk));",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"all\"", position: 37, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create a table with a SQL keyword that is not reserved",
            intent: "Unreserved keywords are column names quoted or not.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk int NOT NULL, \"comment\" int, \"date\" int, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("integer"), T("t"), T("t")],
                            &[T("comment"), T("integer"), T("f"), T("f")],
                            &[T("date"), T("integer"), T("f"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_attribute and pg_index show each column's type, NOT NULL, and primary key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk int NOT NULL, comment int, date int, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test2'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("integer"), T("t"), T("t")],
                            &[T("comment"), T("integer"), T("f"), T("f")],
                            &[T("date"), T("integer"), T("f"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so pg_attribute and pg_index show each column's type, NOT NULL, and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create two table with the same name",
            intent: "Creating a table whose name is taken fails.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test (pk bigint NOT NULL, c1 bigint, c2 bigint, c3 bigint, c4 bigint, c5 bigint, PRIMARY KEY (pk));",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"test\" already exists", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: create like",
            intent: "CREATE TABLE ... LIKE copies the columns, primary key, defaults, and comments.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test1 (pk bigint PRIMARY KEY, c1 bigint DEFAULT 5);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON COLUMN test1.c1 IS 'hi';",
                    expected: Expected::Tag("COMMENT"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's column COMMENT clause is COMMENT ON COLUMN")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (LIKE test1 INCLUDING ALL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's CREATE TABLE ... LIKE is CREATE TABLE ... (LIKE ... INCLUDING ALL)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, i.indisprimary IS TRUE, pg_get_expr(d.adbin, d.adrelid), col_description(a.attrelid, a.attnum) FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum WHERE a.attrelid = 'test2'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("?column?", BOOL), Column("pg_get_expr", TEXT), Column("col_description", TEXT)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("t"), T("t"), Null, Null],
                            &[T("c1"), T("bigint"), T("f"), T("f"), T("5"), T("hi")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints SHOW CREATE TABLE, which is MySQL's, so the catalogs show each column's type, NOT NULL, primary key, default, and comment")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: duplicate column errors",
            intent: "Column names are case-insensitive, so a duplicate name fails in CREATE TABLE, RENAME COLUMN, and ADD COLUMN.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test1 (a bigint PRIMARY KEY, A bigint);",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"a\" specified more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test1 (a bigint PRIMARY KEY, a bigint);",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"a\" specified more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test1 (a bigint PRIMARY KEY, b bigint);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 RENAME COLUMN b TO a;",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"a\" of relation \"test1\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ADD COLUMN A int;",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"a\" of relation \"test1\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 RENAME COLUMN b TO A;",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"a\" of relation \"test1\" already exists", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's CHANGE COLUMN renames and changes a column in one statement, so the port runs the rename, which fails first")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: Can create a temporary table that lasts the length of a session",
            intent: "A temporary table is not one of the database's tables and leaves the working set clean.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE mytemptable(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists, and a temporary table lives in the session's temporary schema")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
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
            name: "sql-create-tables: Temporary tables can have data inserted and retrieved",
            intent: "A temporary table holds rows, and the database's tables and status stay unchanged.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO colors VALUES (1,'red'),(2,'green'),(3,'blue');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM colors;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("color", VARCHAR)],
                        rows: &[
                            &[T("1"), T("red")],
                            &[T("2"), T("green")],
                            &[T("3"), T("blue")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists, and a temporary table lives in the session's temporary schema")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
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
            name: "sql-create-tables: Temporary tables with indexes can have data inserted and retrieved",
            intent: "A temporary table with an index holds rows, and the database's tables and status stay unchanged.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX color_index ON colors (color);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's INDEX clause of CREATE TABLE is CREATE INDEX")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO colors VALUES (1,'red'),(2,'green'),(3,'blue');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM colors;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("color", VARCHAR)],
                        rows: &[
                            &[T("1"), T("red")],
                            &[T("2"), T("green")],
                            &[T("3"), T("blue")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists, and a temporary table lives in the session's temporary schema")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
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
            name: "sql-create-tables: Create temporary table select from another table works",
            intent: "A temporary table created from a query holds its rows and is not listed with the database's tables.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE colors (id int NOT NULL, color VARCHAR(32) NOT NULL, PRIMARY KEY (id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX color_index ON colors (color);",
                    expected: Expected::Tag("CREATE INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's INDEX clause of CREATE TABLE is CREATE INDEX")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO colors VALUES (1,'red'),(2,'green'),(3,'blue');",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE mytemptable AS SELECT * FROM colors;",
                    expected: Expected::Tag("SELECT 3"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's CREATE TABLE ... SELECT is CREATE TABLE ... AS SELECT")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytemptable;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("color", VARCHAR)],
                        rows: &[
                            &[T("1"), T("red")],
                            &[T("2"), T("green")],
                            &[T("3"), T("blue")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("colors")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists, and a temporary table lives in the session's temporary schema")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: You can drop temp tables",
            intent: "A dropped temporary table is gone and leaves the working set clean.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE mytemptable(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE mytemptable;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists, and a temporary table lives in the session's temporary schema")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE mytemptable(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE mytemptable;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytemptable;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"mytemptable\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists, and a temporary table lives in the session's temporary schema")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
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
            name: "sql-create-tables: You can create a temp table of the same name as a normal table. Run it through operations",
            intent: "A temporary table hides a table of the same name until it is dropped, leaving that table untouched.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE goodtable(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE goodtable(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO goodtable VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM goodtable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE goodtable;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("goodtable")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM goodtable;",
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
            name: "sql-create-tables: You can create a normal table even if a temporary table exists with the same name",
            intent: "A table can be created under a temporary table's name, and the temporary table is used until it is dropped.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE goodtable(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO goodtable VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE goodtable(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM goodtable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE goodtable;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("goodtable")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM goodtable;",
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
            name: "sql-create-tables: Alter on a temporary table",
            intent: "A temporary table can be altered and leaves the working set clean.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE goodtable(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE goodtable ADD COLUMN val int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Bug("Dolt skips this test (\"cannot alter temporary tables\"), where Postgres alters a temporary table")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO goodtable VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM goodtable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists, and a temporary table lives in the session's temporary schema")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
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
            name: "sql-create-tables: Creating a foreign key on a temporary table throws an error",
            intent: "A temporary table can't have a foreign key to a table of the database, created with the table or added later.",
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
                    query: "CREATE TEMPORARY TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id), FOREIGN KEY (color) REFERENCES colors(color));",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "constraints on temporary tables may reference only temporary tables", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE objects (id int NOT NULL, name VARCHAR(64) NOT NULL, color VARCHAR(32), PRIMARY KEY(id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE objects ADD FOREIGN KEY (color) REFERENCES colors(color);",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "constraints on temporary tables may reference only temporary tables", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("colors")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists, and a temporary table lives in the session's temporary schema")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: Temporary table supports UNIQUE Index",
            intent: "A temporary table's unique constraint refuses a duplicate value.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE mytemptable (pk int NOT NULL, val int, PRIMARY KEY (pk), UNIQUE (val));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's UNIQUE INDEX clause of CREATE TABLE is UNIQUE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytemptable VALUES (1,1) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's INSERT IGNORE is INSERT ... ON CONFLICT DO NOTHING")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytemptable VALUES (2,1) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's INSERT IGNORE is INSERT ... ON CONFLICT DO NOTHING")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytemptable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls lists the working set's tables, which information_schema.tables lists, and a temporary table lives in the session's temporary schema")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
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
            name: "sql-create-tables: create like with temporary tables",
            intent: "A temporary table can copy a table's definition with LIKE.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mytable (pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE mytemptable (LIKE mytable INCLUDING ALL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's CREATE TABLE ... LIKE is CREATE TABLE ... (LIKE ... INCLUDING ALL)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytemptable VALUES (1,1),(2,1);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytemptable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
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
            name: "sql-create-tables: temporary tables can be queried in a case insensitive way",
            intent: "A temporary table's unquoted name matches in any letter case, and a dropped one is gone.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE myTempTable (pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO myTempTABLE VALUES (1,1),(2,2),(3,3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM MyTempTable WHERE pk = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM myTEMPTable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE myTempTable;",
                    expected: Expected::Tag("DROP TABLE"),
                    dolt: Some(Dolt::Differs("each half of Dolt's test runs in its own dolt sql session, so the port drops the temporary table where the first session ended")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMPORARY TABLE myTempTable (pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO myTempTABLE VALUES (1,1),(2,2),(3,3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE myTempTABLE;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM myTempTable;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"mytemptable\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: tables should not reuse constraint names",
            intent: "A table copied with LIKE has the original's check constraints.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (pk int PRIMARY KEY, val int CHECK (val > 0));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (LIKE t1 INCLUDING ALL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's CREATE TABLE ... LIKE is CREATE TABLE ... (LIKE ... INCLUDING ALL)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(constraint_name), count(DISTINCT constraint_name) FROM information_schema.table_constraints WHERE constraint_type = 'CHECK' AND constraint_schema = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("4"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres' constraint names are unique per table, so LIKE ... INCLUDING ALL keeps the copied constraints' names, and PG18's table_constraints lists NOT NULL constraints as CHECK constraints, where MySQL's check constraint names are unique per schema")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-create-tables: ALTER TABLE ... COMMENT on a table with a primary key",
            intent: "A table with a primary key takes a table comment.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(id int NOT NULL, PRIMARY KEY (id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TABLE t IS 'c';",
                    expected: Expected::Tag("COMMENT"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... COMMENT is COMMENT ON TABLE")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('t'::regclass, 'pg_class');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("c")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so obj_description shows the table's comment")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
