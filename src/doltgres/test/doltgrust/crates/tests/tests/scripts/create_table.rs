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
fn test_create_table() {
    run_scripts(&[
        ScriptTest {
            name: "create table with UTF8 identifiers",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE foo😏(data🍆 TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX idx🍤 ON foo😏(data🍆);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "Insert into foo😏 (data🍆) VALUES ('foo');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT data🍆 FROM foo😏;",
                    expected: Expected::Rows {
                        columns: &[Column("data🍆", TEXT)],
                        rows: &[
                            &[T("foo")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table with primary key",
            assertions: &[
                ScriptTestAssertion {
                    query: "create table employees (    id int8,    last_name text,    first_name text,    primary key(id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into employees (id, last_name, first_name) values (1, 'Doe', 'John');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from employees;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("last_name", TEXT), Column("first_name", TEXT)],
                        rows: &[
                            &[T("1"), T("Doe"), T("John")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname FROM pg_constraint WHERE conrelid = 'employees'::regclass AND contype = 'p';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME)],
                        rows: &[
                            &[T("employees_pkey")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE employees DROP CONSTRAINT employees_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table with primary key, using custom constraint name",
            set_up_script: &[
                "CREATE TABLE users (id SERIAL, name TEXT, CONSTRAINT users_primary_key PRIMARY KEY (id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT conname FROM pg_constraint WHERE conrelid = 'users'::regclass AND contype = 'p';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME)],
                        rows: &[
                            &[T("users_primary_key")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE users DROP CONSTRAINT users_primary_key;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Create table with column default expression using function",
            assertions: &[
                ScriptTestAssertion {
                    query: "create table t1 (pk int primary key, c1 TEXT default length('Hello World!'));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into t1(pk) values (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from t1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT)],
                        rows: &[
                            &[T("1"), T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Create table with table check constraint",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE products (name text, price numeric, discounted_price numeric, CHECK (price > discounted_price));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into products values ('apple', 1.20, 0.80);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into products values ('peach', 1.20, 1.80);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "products" violates check constraint "products_check""#, detail: "Failing row contains (peach, 1.20, 1.80).", schema: "public", table: "products", constraint: "products_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from products;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("price", NUMERIC), Column("discounted_price", NUMERIC)],
                        rows: &[
                            &[T("apple"), T("1.20"), T("0.80")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Create table with column check constraint",
            assertions: &[
                ScriptTestAssertion {
                    query: "create table mytbl (pk int, v1 int constraint v1constraint check (v1 < 100));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into mytbl values (1, 20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into mytbl values (2, 200);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "mytbl" violates check constraint "v1constraint""#, detail: "Failing row contains (2, 200).", schema: "public", table: "mytbl", constraint: "v1constraint", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from mytbl;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("1"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraint with a function",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE mytbl (a text CHECK (length(a) > 2) PRIMARY KEY, b text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into mytbl values ('abc', 'def');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into mytbl values ('de', 'abc');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "mytbl" violates check constraint "mytbl_a_check""#, detail: "Failing row contains (de, abc).", schema: "public", table: "mytbl", constraint: "mytbl_a_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from mytbl;",
                    expected: Expected::Rows {
                        columns: &[Column("a", TEXT), Column("b", TEXT)],
                        rows: &[
                            &[T("abc"), T("def")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Create table with multiple check constraints on a single column",
            assertions: &[
                ScriptTestAssertion {
                    query: "create table mytbl (pk int, v1 int constraint v1constraint check (v1 < 100) check (v1 > 10));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into mytbl values (1, 20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into mytbl values (2, 200);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "mytbl" violates check constraint "v1constraint""#, detail: "Failing row contains (2, 200).", schema: "public", table: "mytbl", constraint: "v1constraint", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into mytbl values (3, 5);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "mytbl" violates check constraint "mytbl_v1_check""#, detail: "Failing row contains (3, 5).", schema: "public", table: "mytbl", constraint: "mytbl_v1_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from mytbl;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("1"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Create table with a check constraints on a single column and a table check constraint",
            assertions: &[
                ScriptTestAssertion {
                    query: "create table mytbl (pk int, v1 int constraint v1constraint check (v1 < 100), check (v1 > 10));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into mytbl values (1, 20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into mytbl values (2, 200);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "mytbl" violates check constraint "v1constraint""#, detail: "Failing row contains (2, 200).", schema: "public", table: "mytbl", constraint: "v1constraint", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into mytbl values (3, 5);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "mytbl" violates check constraint "mytbl_v1_check""#, detail: "Failing row contains (3, 5).", schema: "public", table: "mytbl", constraint: "mytbl_v1_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from mytbl;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("1"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table with generated column",
            set_up_script: &[
                "create table t1 (a int primary key, b int, c int generated always as (a + b) stored);",
                "insert into t1 (a, b) values (1, 2);",
                "create table t2 (a int primary key, b int, c int generated always as (b * 10) stored);",
                "insert into t2 (a, b) values (1, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select * from t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from t2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        // Changed from the Go test: concat is not immutable in Postgres, so it cannot define a generated column; || is.
        ScriptTest {
            name: "create table with function in generated column",
            set_up_script: &[
                "create table t1 (a varchar(10) primary key, b varchar(10), c varchar(20) generated always as (a || b) stored);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "insert into t1 (a, b) values ('foo', 'bar');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", VARCHAR), Column("b", VARCHAR), Column("c", VARCHAR)],
                        rows: &[
                            &[T("foo"), T("bar"), T("foobar")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "generated column with complex expression",
            set_up_script: &[
                r#"create table t1 (a varchar(10) primary key,
				b varchar(20) generated always as 
				    ((
				        ("substring"(TRIM(BOTH FROM a), '([^ ]+)$'::text) || ' '::text)
				          || "substring"(TRIM(BOTH FROM a), '^([^ ]+)'::text)
				    )) stored
				);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "insert into t1 (a) values (' foo ');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T(" foo "), T("foo foo")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "generated column with reference to another column",
            set_up_script: &[
                r#"create table t1 (
    			a varchar(10) primary key,
    			b varchar(20),
				  b_not_null bool generated always as ((b is not null)) stored
				);"#,
                "insert into t1 (a, b) values ('foo', 'bar');",
                "insert into t1 (a) values ('foo2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select * from t1 order by a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", VARCHAR), Column("b", VARCHAR), Column("b_not_null", BOOL)],
                        rows: &[
                            &[T("foo"), T("bar"), T("t")],
                            &[T("foo2"), Null, T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "generated column with space in column name",
            set_up_script: &[
                r#"create table t1 (
    			a varchar(10) primary key,
    			"b 2" varchar(20),
				  b_not_null bool generated always as (("b 2" is not null)) stored
				);"#,
                r#"insert into t1 (a, "b 2") values ('foo', 'bar');"#,
                "insert into t1 (a) values ('foo2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select * from t1 order by a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", VARCHAR), Column("b 2", VARCHAR), Column("b_not_null", BOOL)],
                        rows: &[
                            &[T("foo"), T("bar"), T("t")],
                            &[T("foo2"), Null, T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary key GENERATED ALWAYS AS IDENTITY",
            set_up_script: &[
                r#"create table t1 (
    			a BIGINT NOT NULL PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
				  b varchar(100)
				);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "insert into t1 (b) values ('foo') returning a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into t1 (a, b) values (2, 'foo') returning a;",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: r#"cannot insert a non-DEFAULT value into column "a""#, detail: r#"Column "a" is an identity column defined as GENERATED ALWAYS."#, hint: "Use OVERRIDING SYSTEM VALUE to override.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table with default value",
            set_up_script: &[
                "create table t1 (a varchar(10) primary key, b varchar(10) default (concat('foo', 'bar')));",
                "insert into t1 (a) values ('abc');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select * from t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("abc"), T("foobar")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table with collation",
            set_up_script: &[
                r#"CREATE TABLE collate_test1 (
    a int,
        b text COLLATE "en-x-icu" NOT NULL
        )"#,
                "insert into collate_test1 (a, b) values (1, 'foo');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select * from collate_test1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("foo")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "inline comments",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE inline_comments (
	a int,
	b int,
	c int, -- comment on end of line
	CONSTRAINT check_b CHECK (b IS NULL OR b = 'a'),
	CONSTRAINT check_a CHECK (a IS NOT NULL AND a = 7)
);"#,
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "a""#, position: 126, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE block_comments (
	a int,
	b /* block comment */ /* one more thing */ int, -- comment on end of line
	c int, -- comment on end of line /* block comment */
	CONSTRAINT check_b CHECK (b IS NULL OR b = 'a'),
	CONSTRAINT check_a CHECK (a IS NOT NULL AND a = 7)
);"#,
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "a""#, position: 212, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create temporary table with serial column",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE temp (id serial primary key)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "table with check constraint with ANY expression",
            set_up_script: &[
                r#"CREATE TABLE location (
    id integer NOT NULL,
    name character varying(100) NOT NULL,
    type character varying(100),
    CONSTRAINT location_type_check CHECK (((type)::text = ANY ((ARRAY['Внутренни'::character varying, 'Покупатель'::character varying, 'Поставщик'::character varying])::text[])))
);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "insert into location values (1, 'Склад Москва', 'Внутренни'), (2, 'Склад Спб', null);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM location;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR), Column("type", VARCHAR)],
                        rows: &[
                            &[T("1"), T("Склад Москва"), T("Внутренни")],
                            &[T("2"), T("Склад Спб"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Table names must be unique across all relation types",
            set_up_script: &[
                "CREATE TABLE existing_tbl (pk int PRIMARY KEY, v1 int);",
                "CREATE SEQUENCE seq1;",
                "CREATE VIEW view1 AS SELECT pk FROM existing_tbl;",
                "CREATE INDEX idx1 ON existing_tbl (v1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE existing_tbl (c1 int);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "existing_tbl" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE IF NOT EXISTS existing_tbl (c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "42P07", message: r#"relation "existing_tbl" already exists, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE seq1 (c1 int);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "seq1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE IF NOT EXISTS seq1 (c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "42P07", message: r#"relation "seq1" already exists, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE view1 (c1 int);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "view1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE IF NOT EXISTS view1 (c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "42P07", message: r#"relation "view1" already exists, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE idx1 (c1 int);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "idx1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE IF NOT EXISTS idx1 (c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "42P07", message: r#"relation "idx1" already exists, skipping"#, ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nested parentheses are kept in default and check expressions",
            set_up_script: &[
                "CREATE TABLE t3324 (a INT PRIMARY KEY, b INT DEFAULT (1 + 1) * 2, c INT DEFAULT 2 * (3 + 1) + 1, d INT DEFAULT -(1 + 1), e INT GENERATED ALWAYS AS ((a + 1) * 2) STORED, f INT DEFAULT (((1 + 2)) * ((3))), g INT DEFAULT 10 - (4 - 1), h INT DEFAULT (2 + 3) % 4, i BOOLEAN DEFAULT (NOT (1 = 1 AND 2 = 2)), j INT DEFAULT abs(1 - 3) * 2, k TEXT DEFAULT ('a' || 'b') || 'c', l INT DEFAULT (1 + 2)::INT * 2, m INT DEFAULT -(-1), n BOOLEAN DEFAULT ((1 IS NULL) IS NULL), CONSTRAINT chk3324 CHECK (((a + 1) * 2) > 3), CONSTRAINT chk3324b CHECK (NOT (a = 0 OR a + 1 = 0) AND a - (a - 1) = 1));",
                "INSERT INTO t3324 (a) VALUES (1);",
                "ALTER TABLE t3324 ADD COLUMN o INT DEFAULT (1 + 1) * 2;",
                "ALTER TABLE t3324 ALTER COLUMN o SET DEFAULT 2 * (1 + 1) + 1;",
                "ALTER TABLE t3324 ADD CONSTRAINT chk3324c CHECK ((a * 2) - 1 > 0);",
                "INSERT INTO t3324 (a) VALUES (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t3324 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4), Column("d", INT4), Column("e", INT4), Column("f", INT4), Column("g", INT4), Column("h", INT4), Column("i", BOOL), Column("j", INT4), Column("k", TEXT), Column("l", INT4), Column("m", INT4), Column("n", BOOL), Column("o", INT4)],
                        rows: &[
                            &[T("1"), T("4"), T("9"), T("-2"), T("4"), T("9"), T("7"), T("1"), T("f"), T("4"), T("abc"), T("6"), T("1"), T("f"), T("4")],
                            &[T("2"), T("4"), T("9"), T("-2"), T("6"), T("9"), T("7"), T("1"), T("f"), T("4"), T("abc"), T("6"), T("1"), T("f"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3324 (a) VALUES (0);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "t3324" violates check constraint "chk3324""#, detail: "Failing row contains (0, 4, 9, -2, 2, 9, 7, 1, f, 4, abc, 6, 1, f, 5).", schema: "public", table: "t3324", constraint: "chk3324", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nested parentheses in generated and check expressions survive ALTER TABLE",
            set_up_script: &[
                "CREATE TABLE t3324b (a INT NOT NULL, b INT DEFAULT (1 + 1) * 2, c INT GENERATED ALWAYS AS ((a + 1) * 2) STORED, d INT GENERATED ALWAYS AS (2 * (a + 1) - (a - 1)) STORED, CHECK ((a + 1) * 2 > 3));",
                "INSERT INTO t3324b (a) VALUES (1);",
                "ALTER TABLE t3324b ADD PRIMARY KEY (a);",
                "INSERT INTO t3324b (a) VALUES (2);",
                "ALTER TABLE t3324b ADD COLUMN e INT DEFAULT (3 + 4) * 5;",
                "INSERT INTO t3324b (a) VALUES (3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t3324b (a) VALUES (0);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "t3324b" violates check constraint "t3324b_a_check""#, detail: "Failing row contains (0, 4, 2, 3, 35).", schema: "public", table: "t3324b", constraint: "t3324b_a_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3324b ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4), Column("d", INT4), Column("e", INT4)],
                        rows: &[
                            &[T("1"), T("4"), T("4"), T("4"), T("35")],
                            &[T("2"), T("4"), T("6"), T("5"), T("35")],
                            &[T("3"), T("4"), T("8"), T("6"), T("35")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nested parentheses are kept on the right side and under unary minus in generated expressions",
            set_up_script: &[
                "CREATE TABLE t3324c (a INT, b INT GENERATED ALWAYS AS (-(a + 1)) STORED, c INT GENERATED ALWAYS AS (2 * (a + 1)) STORED, d INT GENERATED ALWAYS AS (a - (1 - 2)) STORED, e INT DEFAULT ((1 + 2) * 3));",
                "INSERT INTO t3324c (a) VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t3324c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4), Column("d", INT4), Column("e", INT4)],
                        rows: &[
                            &[T("1"), T("-2"), T("4"), T("2"), T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nested parentheses are kept around LIKE, IN, subscripts, and double negation",
            set_up_script: &[
                "CREATE TABLE t3324d (a TEXT, b BOOLEAN DEFAULT (('abc' LIKE 'a%') IS NOT NULL), c BOOLEAN DEFAULT ((1 + 1) IN (2, 3)), d INT DEFAULT ((ARRAY[1] || ARRAY[2])[1]), e INT DEFAULT -(-1), f INT DEFAULT (- (- 2)), CHECK ((a || 'x') LIKE 'a%'));",
                "INSERT INTO t3324d (a) VALUES ('a');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t3324d (a) VALUES ('b');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "t3324d" violates check constraint "t3324d_a_check""#, detail: "Failing row contains (b, t, t, 1, 1, 2).", schema: "public", table: "t3324d", constraint: "t3324d_a_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3324d;",
                    expected: Expected::Rows {
                        columns: &[Column("a", TEXT), Column("b", BOOL), Column("c", BOOL), Column("d", INT4), Column("e", INT4), Column("f", INT4)],
                        rows: &[
                            &[T("a"), T("t"), T("t"), T("1"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "nested parentheses are kept in check constraints using NOT, AND, OR, BETWEEN, CAST, and LIKE",
            set_up_script: &[
                "CREATE TABLE tc3324 (a INT, b INT, CONSTRAINT c1 CHECK (NOT (a = 0 OR a + 1 = 0) AND a - (a - 1) = 1), CONSTRAINT c2 CHECK ((a BETWEEN 1 AND 10) OR (b IS NULL)), CONSTRAINT c3 CHECK (((a + 1) * 2) > 3), CONSTRAINT c4 CHECK (NOT ((a + b) > 100)), CONSTRAINT c5 CHECK (CAST(a + 1 AS INT) > 0), CONSTRAINT c6 CHECK ((a || '') NOT LIKE 'x%'));",
                "INSERT INTO tc3324 VALUES (1, NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO tc3324 VALUES (0, 1);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "tc3324" violates check constraint "c1""#, detail: "Failing row contains (0, 1).", schema: "public", table: "tc3324", constraint: "c1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tc3324 VALUES (50, 60);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "tc3324" violates check constraint "c2""#, detail: "Failing row contains (50, 60).", schema: "public", table: "tc3324", constraint: "c2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tc3324 VALUES (-1, 5);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "tc3324" violates check constraint "c1""#, detail: "Failing row contains (-1, 5).", schema: "public", table: "tc3324", constraint: "c1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tc3324 VALUES (5, 96);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "tc3324" violates check constraint "c4""#, detail: "Failing row contains (5, 96).", schema: "public", table: "tc3324", constraint: "c4", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tc3324 VALUES (2, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tc3324 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "generated column and default with nested parentheses match the equivalent SELECT expressions",
            set_up_script: &[
                "CREATE TABLE tx3324 (a INT, b INT GENERATED ALWAYS AS ((a + 1) * 2) STORED, c INT DEFAULT ((1 + 2) * 3));",
                "INSERT INTO tx3324 (a) VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT a, b, (a + 1) * 2 AS expected_b, c, (1 + 2) * 3 AS expected_c FROM tx3324;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("expected_b", INT4), Column("c", INT4), Column("expected_c", INT4)],
                        rows: &[
                            &[T("1"), T("4"), T("4"), T("9"), T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "named column constraints",
            set_up_script: &[
                "CREATE TABLE t3332_issue (id INTEGER CONSTRAINT id_required NOT NULL, v TEXT);",
                "CREATE TABLE t3332 (id INT CONSTRAINT id_nn NOT NULL, u INT CONSTRAINT u_uni UNIQUE, d INT CONSTRAINT d_def DEFAULT 5, n INT CONSTRAINT n_null NULL, PRIMARY KEY (id));",
                "ALTER TABLE t3332 ADD COLUMN w INT CONSTRAINT w_nn NOT NULL DEFAULT 1 CONSTRAINT w_uni UNIQUE;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_name = 't3332_issue';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("t3332_issue")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3332 (id, u) VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3332 (id, u, w) VALUES (2, 2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3332 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("u", INT4), Column("d", INT4), Column("n", INT4), Column("w", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("5"), Null, T("1")],
                            &[T("2"), T("2"), T("5"), Null, T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 't3332' ORDER BY indexname;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("t3332_pkey")],
                            &[T("u_uni")],
                            &[T("w_uni")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, contype FROM pg_constraint WHERE conrelid = 't3332'::regclass ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("contype", CHAR)],
                        rows: &[
                            &[T("id_nn"), T("n")],
                            &[T("t3332_pkey"), T("p")],
                            &[T("u_uni"), T("u")],
                            &[T("w_nn"), T("n")],
                            &[T("w_uni"), T("u")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3332 (id, u, w) VALUES (3, 1, 3);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "u_uni""#, detail: "Key (u)=(1) already exists.", schema: "public", table: "t3332", constraint: "u_uni", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3332 (id, u, w) VALUES (3, 3, 2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "w_uni""#, detail: "Key (w)=(2) already exists.", schema: "public", table: "t3332", constraint: "w_uni", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3332 (id, u, w) VALUES (NULL, 4, 4);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"null value in column "id" of relation "t3332" violates not-null constraint"#, detail: "Failing row contains (null, 4, 5, null, 4).", schema: "public", table: "t3332", column: "id", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DEFERRABLE constraints",
            set_up_script: &[
                "CREATE TABLE p (id INTEGER PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE a1 (x INTEGER REFERENCES p(id) DEFERRABLE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a2 (x INTEGER REFERENCES p(id) INITIALLY DEFERRED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a3 (x INTEGER REFERENCES p(id) DEFERRABLE INITIALLY IMMEDIATE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a4 (x INTEGER REFERENCES p(id) NOT DEFERRABLE INITIALLY IMMEDIATE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a5 (x INTEGER REFERENCES p(id) NOT DEFERRABLE INITIALLY DEFERRED);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "constraint declared INITIALLY DEFERRED must be DEFERRABLE", position: 60, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a6 (x INTEGER UNIQUE DEFERRABLE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a7 (x INTEGER PRIMARY KEY DEFERRABLE INITIALLY IMMEDIATE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a8 (x INTEGER UNIQUE NOT DEFERRABLE INITIALLY IMMEDIATE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a9 (x INTEGER CHECK (x > 0) DEFERRABLE);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "misplaced DEFERRABLE clause", position: 42, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a10 (x INTEGER NOT NULL DEFERRABLE);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "misplaced DEFERRABLE clause", position: 38, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a11 (x INTEGER DEFAULT 1 DEFERRABLE);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "misplaced DEFERRABLE clause", position: 39, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a12 (x INTEGER NULL NOT DEFERRABLE);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "misplaced NOT DEFERRABLE clause", position: 34, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a13 (x INTEGER CHECK (x > 0) INITIALLY IMMEDIATE);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "misplaced INITIALLY IMMEDIATE clause", position: 43, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a14 (x INTEGER CHECK (x > 0) INITIALLY DEFERRED);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "misplaced INITIALLY DEFERRED clause", position: 43, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a15 (x INTEGER GENERATED ALWAYS AS (1) STORED DEFERRABLE);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "misplaced DEFERRABLE clause", position: 60, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b1 (x INTEGER, FOREIGN KEY (x) REFERENCES p(id) DEFERRABLE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b2 (x INTEGER, CONSTRAINT b2u UNIQUE (x) INITIALLY DEFERRED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b3 (x INTEGER, PRIMARY KEY (x) NOT DEFERRABLE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b4 (x INTEGER, FOREIGN KEY (x) REFERENCES p(id) INITIALLY IMMEDIATE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b5 (x INTEGER, UNIQUE (x) NOT DEFERRABLE INITIALLY DEFERRED);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "constraint declared INITIALLY DEFERRED must be DEFERRABLE", position: 55, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b6 (x INTEGER, CHECK (x > 0) DEFERRABLE);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "CHECK constraints cannot be marked DEFERRABLE", position: 43, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b7 (x INTEGER, CHECK (x > 0) INITIALLY DEFERRED);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "CHECK constraints cannot be marked DEFERRABLE", position: 43, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b8 (x INTEGER, CHECK (x > 0) NOT DEFERRABLE INITIALLY DEFERRED);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "constraint declared INITIALLY DEFERRED must be DEFERRABLE", position: 58, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b9 (x INTEGER, CHECK (x > 0) NOT DEFERRABLE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b10 (x INTEGER, CHECK (x > 0) INITIALLY IMMEDIATE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DEFERRABLE constraints are stored",
            set_up_script: &[
                "CREATE TABLE p (id INTEGER PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE a1 (x INTEGER REFERENCES p(id) DEFERRABLE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a2 (x INTEGER REFERENCES p(id) INITIALLY DEFERRED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a3 (x INTEGER REFERENCES p(id) DEFERRABLE INITIALLY IMMEDIATE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a6 (x INTEGER UNIQUE DEFERRABLE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a7 (x INTEGER PRIMARY KEY DEFERRABLE INITIALLY IMMEDIATE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b1 (x INTEGER, FOREIGN KEY (x) REFERENCES p(id) DEFERRABLE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE b2 (x INTEGER, CONSTRAINT b2u UNIQUE (x) INITIALLY DEFERRED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, condeferrable, condeferred FROM pg_constraint WHERE connamespace = 'public'::regnamespace AND condeferrable ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("condeferrable", BOOL), Column("condeferred", BOOL)],
                        rows: &[
                            &[T("a1_x_fkey"), T("t"), T("f")],
                            &[T("a2_x_fkey"), T("t"), T("t")],
                            &[T("a3_x_fkey"), T("t"), T("f")],
                            &[T("a6_x_key"), T("t"), T("f")],
                            &[T("a7_pkey"), T("t"), T("f")],
                            &[T("b1_x_fkey"), T("t"), T("f")],
                            &[T("b2u"), T("t"), T("t")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_create_table_inherit() {
    run_scripts(&[
        ScriptTest {
            name: "Create table with inheritance",
            set_up_script: &[
                "create table t1 (a int);",
                "create table t2 (b int);",
                "create table t3 (c int);",
                "create table t11 (a int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "create table t4 (d int) inherits (t1, t2, t3);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into t4(a, b, c, d) values (1, 2, 3, 4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from t4;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "create table t111 () inherits (t1, t11);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"merging multiple inherited definitions of column "a""#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into t111(a) values (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from t111;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "create table t1t1 (a int) inherits (t1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"merging column "a" with inherited definition"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into t1t1(a) values (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from t1t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "create table TT1t1 (A int) inherits (t1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"merging column "a" with inherited definition"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into TT1t1(a) values (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from TT1t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
    ]);
}

#[test]
fn test_column_default_rules() {
    run_scripts(&[
        ScriptTest {
            name: "expressions that defaults cannot be",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (a INT, b INT DEFAULT (a));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot use column reference in DEFAULT expression", position: 40, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (a INT, b INT DEFAULT 'x'::text);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"column "b" is of type integer but default expression is of type text"#, hint: "You will need to rewrite or cast the expression.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3 (a INT, b INT DEFAULT sum(1));",
                    expected: Expected::Error(Diagnostic { code: "42803", message: "aggregate functions are not allowed in DEFAULT expressions", position: 39, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t4 (a INT, b INT DEFAULT row_number() OVER ());",
                    expected: Expected::Error(Diagnostic { code: "42P20", message: "window functions are not allowed in DEFAULT expressions", position: 39, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t5 (a INT, b INT DEFAULT (SELECT 1));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot use subquery in DEFAULT expression", position: 39, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t6 (a INT, b TEXT DEFAULT 5);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t7 (a INT, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t7 ALTER COLUMN b SET DEFAULT (a);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot use column reference in DEFAULT expression", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t7 ALTER COLUMN b SET DEFAULT t7.a;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot use column reference in DEFAULT expression", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t7 ALTER COLUMN b SET DEFAULT 'x'::text;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"column "b" is of type integer but default expression is of type text"#, hint: "You will need to rewrite or cast the expression.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t7 ALTER COLUMN b SET DEFAULT 'x';",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "x""#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t7 ALTER COLUMN b SET DEFAULT generate_series(1, 2);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "set-returning functions are not allowed in DEFAULT expressions", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t7 ADD COLUMN c INT DEFAULT a;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot use column reference in DEFAULT expression", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t8 (a INT, b INT DEFAULT 'x');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "x""#, position: 39, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t9 (a INT DEFAULT 1 + 1, b INT DEFAULT now()::date - '2020-01-01'::date, c VARCHAR(2) DEFAULT 'abc');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t9 (a) VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "22001", message: "value too long for type character varying(2)", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t9 (a, c) VALUES (1, 'ab');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, c FROM t9;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("1"), T("ab")],
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
fn test_generated_column_rules() {
    run_scripts(&[
        ScriptTest {
            name: "stored generated columns",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE g (a INT PRIMARY KEY, b INT GENERATED ALWAYS AS (a * 2 + 1) STORED, c TEXT DEFAULT 'x', d TEXT GENERATED ALWAYS AS (upper(c)) STORED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO g (a) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO g VALUES (2, 5);",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: r#"cannot insert a non-DEFAULT value into column "b""#, detail: r#"Column "b" is a generated column."#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO g VALUES (3, DEFAULT, 'y');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO g (a, b) VALUES (4, 7);",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: r#"cannot insert a non-DEFAULT value into column "b""#, detail: r#"Column "b" is a generated column."#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE g SET b = 9 WHERE a = 1;",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: r#"column "b" can only be updated to DEFAULT"#, detail: r#"Column "b" is a generated column."#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE g SET b = DEFAULT, c = 'z' WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM g ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("d", TEXT)],
                        rows: &[
                            &[T("1"), T("3"), T("z"), T("Z")],
                            &[T("3"), T("7"), T("y"), T("Y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE g ADD COLUMN e INT GENERATED ALWAYS AS (a + 100) STORED;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, e FROM g ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("e", INT4)],
                        rows: &[
                            &[T("1"), T("101")],
                            &[T("3"), T("103")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attgenerated FROM pg_attribute WHERE attrelid = 'g'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attgenerated", CHAR)],
                        rows: &[
                            &[T("a"), T("")],
                            &[T("b"), T("s")],
                            &[T("c"), T("")],
                            &[T("d"), T("s")],
                            &[T("e"), T("s")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, is_generated FROM information_schema.columns WHERE table_name = 'g' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("is_generated", VARCHAR)],
                        rows: &[
                            &[T("a"), T("NEVER")],
                            &[T("b"), T("ALWAYS")],
                            &[T("c"), T("NEVER")],
                            &[T("d"), T("ALWAYS")],
                            &[T("e"), T("ALWAYS")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO g (a, b) VALUES (10, DEFAULT) RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("d", TEXT), Column("e", INT4)],
                        rows: &[
                            &[T("10"), T("21"), T("x"), T("X"), T("110")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO g (a) SELECT 20 RETURNING a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("20"), T("41")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO g (a, b) SELECT 30, 1;",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: r#"cannot insert a non-DEFAULT value into column "b""#, detail: r#"Column "b" is a generated column."#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "expressions that generated columns cannot have",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE g2 (a INT, b INT GENERATED ALWAYS AS (a) STORED DEFAULT 1);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"both default and generation expression specified for column "b" of table "g2""#, position: 62, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE g2b (a INT, b INT DEFAULT 1 GENERATED ALWAYS AS (a) STORED);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"both default and generation expression specified for column "b" of table "g2b""#, position: 42, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE g3 (a INT, b INT GENERATED ALWAYS AS (b + 1) STORED);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"cannot use generated column "b" in column generation expression"#, detail: "A generated column cannot reference another generated column.", position: 52, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE g4 (a INT, b INT GENERATED ALWAYS AS (a + 1) STORED, c INT GENERATED ALWAYS AS (b + 1) STORED);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"cannot use generated column "b" in column generation expression"#, detail: "A generated column cannot reference another generated column.", position: 94, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE g5 (a INT, b TIMESTAMPTZ GENERATED ALWAYS AS (now()) STORED);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "generation expression is not immutable", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE g7 (a INT, b INT GENERATED ALWAYS AS (sum(a)) STORED);",
                    expected: Expected::Error(Diagnostic { code: "42803", message: "aggregate functions are not allowed in column generation expressions", position: 52, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE g8 (a INT, b INT GENERATED ALWAYS AS ((SELECT 1)) STORED);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot use subquery in column generation expression", position: 52, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a generated column can't reference another, virtual or stored, made with the table or added after",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE gv (a INT, b INT GENERATED ALWAYS AS (a * 2) VIRTUAL, c INT GENERATED ALWAYS AS (b + 1) VIRTUAL);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"cannot use generated column "b" in column generation expression"#, detail: "A generated column cannot reference another generated column.", position: 95, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ga (a INT, b INT GENERATED ALWAYS AS (a * 2) STORED);",
                    expected: Expected::Ok,
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ga ADD COLUMN c INT GENERATED ALWAYS AS (b + 1) STORED;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"cannot use generated column "b" in column generation expression"#, detail: "A generated column cannot reference another generated column.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE gb (a INT, b INT GENERATED ALWAYS AS (a * 2) VIRTUAL);",
                    expected: Expected::Ok,
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE gb ADD COLUMN c INT GENERATED ALWAYS AS (b + 1) STORED;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: r#"cannot use generated column "b" in column generation expression"#, detail: "A generated column cannot reference another generated column.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_constraint_attribute_rules() {
    run_scripts(&[
        ScriptTest {
            name: "constraint attributes and DEFAULT VALUES",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE a15 (x INTEGER GENERATED ALWAYS AS (1) STORED DEFERRABLE);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "misplaced DEFERRABLE clause", position: 60, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a16 (x INTEGER NOT NULL NOT DEFERRABLE);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "misplaced NOT DEFERRABLE clause", position: 38, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a17 (x INTEGER DEFAULT 1 INITIALLY DEFERRED);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "misplaced INITIALLY DEFERRED clause", position: 39, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a19 (x INTEGER DEFERRABLE);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "misplaced DEFERRABLE clause", position: 29, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a20 (x INTEGER UNIQUE INITIALLY DEFERRED NOT DEFERRABLE);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "constraint declared INITIALLY DEFERRED must be DEFERRABLE", position: 55, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a21 (x INTEGER UNIQUE DEFERRABLE DEFERRABLE);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed", position: 47, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dv (a INT DEFAULT 1, b INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dv DEFAULT VALUES;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dv DEFAULT VALUES RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_inherited_column_rules() {
    run_scripts(&[
        ScriptTest {
            name: "Inherited column merging",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE p1 (a INT NOT NULL DEFAULT 7, b TEXT, CONSTRAINT p1_b CHECK (b <> 'x'));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE p2 (a TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE c1 () INHERITS (p1, p2);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"inherited column "a" has a type conflict"#, detail: "integer versus text", ..E }),
                    notices: &[Diagnostic { code: "00000", message: r#"merging multiple inherited definitions of column "a""#, ..N }],
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE c2 (a TEXT) INHERITS (p1);",
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"column "a" has a type conflict"#, detail: "integer versus text", ..E }),
                    notices: &[Diagnostic { code: "00000", message: r#"merging column "a" with inherited definition"#, ..N }],
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE c3 (c INT, b TEXT DEFAULT 'z') INHERITS (p1);",
                    expected: Expected::Tag("CREATE TABLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"merging column "b" with inherited definition"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c3 (c) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM c3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("7"), T("z"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c3 (a) VALUES (NULL);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"null value in column "a" of relation "c3" violates not-null constraint"#, detail: "Failing row contains (null, z, null).", schema: "public", table: "c3", column: "a", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c3 (b) VALUES ('x');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "c3" violates check constraint "p1_b""#, detail: "Failing row contains (7, x, null).", schema: "public", table: "c3", constraint: "p1_b", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE c4 () INHERITS (missing);",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "missing" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Parent scans include inherited rows",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE p3 (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE c5 (b INT) INHERITS (p3);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c5 VALUES (1, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM p3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
    ]);
}

#[test]
fn test_check_creation_rules() {
    run_scripts(&[
        ScriptTest {
            name: "check constraints bound at creation",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ck1 (a int, b int, CONSTRAINT check_b CHECK (b IS NULL OR b = 'a'));",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "a""#, position: 76, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ck2 (a int CHECK (a + 1));",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of CHECK must be type boolean, not type integer", position: 32, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ck3 (a int CHECK (nosuch > 0));",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "nosuch" does not exist"#, position: 32, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ck4 (a int CHECK (a > (SELECT 1)));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot use subquery in check constraint", position: 36, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ck5 (a int CHECK (sum(a) > 0));",
                    expected: Expected::Error(Diagnostic { code: "42803", message: "aggregate functions are not allowed in check constraints", position: 32, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ck6 (a int CHECK (a > 0), b text CHECK (b <> ''));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ck6 VALUES (1, 'x');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ck6 VALUES (0, 'x');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "ck6" violates check constraint "ck6_a_check""#, detail: "Failing row contains (0, x).", schema: "public", table: "ck6", constraint: "ck6_a_check", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_vacuum_comment_and_system_check_rules() {
    run_scripts(&[
        ScriptTest {
            name: "vacuum statements",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE vt (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM ANALYZE vt;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM nosuch;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "nosuch" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM FULL vt (a);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "ANALYZE option must be specified when a column list is provided", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM vt (a);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "ANALYZE option must be specified when a column list is provided", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM (ANALYZE) vt (a);",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "comments on tables and columns",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ct (id int, v text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TABLE ct IS 'tbl';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON COLUMN ct.id IS 'col id';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON COLUMN ct.nosuch IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "nosuch" of relation "ct" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TABLE nosuch IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "nosuch" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON COLUMN nosuch.id IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "nosuch" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON COLUMN id IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "column name must be qualified", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('ct'::regclass, 'pg_class'), col_description('ct'::regclass, 1), col_description('ct'::regclass, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT), Column("col_description", TEXT), Column("col_description", TEXT)],
                        rows: &[
                            &[T("tbl"), T("col id"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT objsubid, description FROM pg_description WHERE objoid = 'ct'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("objsubid", INT4), Column("description", TEXT)],
                        rows: &[
                            &[T("0"), T("tbl")],
                            &[T("1"), T("col id")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TABLE ct IS NULL;",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON COLUMN ct.id IS '';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('ct'::regclass, 'pg_class'), col_description('ct'::regclass, 1);",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT), Column("col_description", TEXT)],
                        rows: &[
                            &[Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "check constraints over system columns",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE sys_col_check (city text, is_capital bool, CHECK (NOT (is_capital AND tableoid::regclass::text = 'sys_col_check')));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sys_col_check VALUES ('Seattle', false);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sys_col_check VALUES ('Olympia', true);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "sys_col_check" violates check constraint "sys_col_check_check""#, detail: "Failing row contains (Olympia, t).", schema: "public", table: "sys_col_check", constraint: "sys_col_check_check", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM sys_col_check;",
                    expected: Expected::Rows {
                        columns: &[Column("city", TEXT), Column("is_capital", BOOL)],
                        rows: &[
                            &[T("Seattle"), T("f")],
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
fn test_create_table_like_rules() {
    run_scripts(&[
        ScriptTest {
            name: "create table like",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE src (id int PRIMARY KEY, a text NOT NULL DEFAULT 'x', b int CHECK (b > 0), c int GENERATED ALWAYS AS (b * 2) STORED, u int UNIQUE);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l1 (LIKE src);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l2 (LIKE src INCLUDING ALL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l3 (extra int, LIKE src INCLUDING DEFAULTS INCLUDING CONSTRAINTS, more text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l4 (LIKE nosuch);",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "nosuch" does not exist"#, position: 23, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, pg_get_expr(d.adbin, d.adrelid) FROM pg_attribute a JOIN pg_class c ON c.oid = a.attrelid LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum WHERE c.relname IN ('l1','l2','l3') AND a.attnum > 0 ORDER BY 1, a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("pg_get_expr", TEXT)],
                        rows: &[
                            &[T("l1"), T("id"), T("integer"), T("t"), Null],
                            &[T("l1"), T("a"), T("text"), T("t"), Null],
                            &[T("l1"), T("b"), T("integer"), T("f"), Null],
                            &[T("l1"), T("c"), T("integer"), T("f"), Null],
                            &[T("l1"), T("u"), T("integer"), T("f"), Null],
                            &[T("l2"), T("id"), T("integer"), T("t"), Null],
                            &[T("l2"), T("a"), T("text"), T("t"), T("'x'::text")],
                            &[T("l2"), T("b"), T("integer"), T("f"), Null],
                            &[T("l2"), T("c"), T("integer"), T("f"), T("(b * 2)")],
                            &[T("l2"), T("u"), T("integer"), T("f"), Null],
                            &[T("l3"), T("extra"), T("integer"), T("f"), Null],
                            &[T("l3"), T("id"), T("integer"), T("t"), Null],
                            &[T("l3"), T("a"), T("text"), T("t"), T("'x'::text")],
                            &[T("l3"), T("b"), T("integer"), T("f"), Null],
                            &[T("l3"), T("c"), T("integer"), T("f"), Null],
                            &[T("l3"), T("u"), T("integer"), T("f"), Null],
                            &[T("l3"), T("more"), T("text"), T("f"), Null],
                        ],
                        tag: "SELECT 17",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conrelid::regclass, conname, contype, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid IN ('l1'::regclass, 'l2'::regclass, 'l3'::regclass) ORDER BY conrelid::regclass::text, conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conrelid", REGCLASS), Column("conname", NAME), Column("contype", CHAR), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("l1"), T("src_a_not_null"), T("n"), T("NOT NULL a")],
                            &[T("l1"), T("src_id_not_null"), T("n"), T("NOT NULL id")],
                            &[T("l2"), T("l2_pkey"), T("p"), T("PRIMARY KEY (id)")],
                            &[T("l2"), T("l2_u_key"), T("u"), T("UNIQUE (u)")],
                            &[T("l2"), T("src_a_not_null"), T("n"), T("NOT NULL a")],
                            &[T("l2"), T("src_b_check"), T("c"), T("CHECK ((b > 0))")],
                            &[T("l2"), T("src_id_not_null"), T("n"), T("NOT NULL id")],
                            &[T("l3"), T("src_a_not_null"), T("n"), T("NOT NULL a")],
                            &[T("l3"), T("src_b_check"), T("c"), T("CHECK ((b > 0))")],
                            &[T("l3"), T("src_id_not_null"), T("n"), T("NOT NULL id")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO l1 (id, a) VALUES (1, 'q');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO l2 (id, b) VALUES (1, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM l2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", TEXT), Column("b", INT4), Column("c", INT4), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("x"), T("5"), T("10"), Null],
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
fn test_temporary_tables() {
    run_scripts(&[
        ScriptTest {
            name: "temporary tables",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE perm (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO perm VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE perm (b text, id serial PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO perm (b) VALUES ('x'), ('y');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM perm ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT), Column("id", INT4)],
                        rows: &[
                            &[T("x"), T("1")],
                            &[T("y"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM public.perm;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_temp.perm ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT), Column("id", INT4)],
                        rows: &[
                            &[T("x"), T("1")],
                            &[T("y"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relpersistence FROM pg_class WHERE relname IN ('perm', 'perm_pkey', 'perm_id_seq') ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("perm"), T("p")],
                            &[T("perm"), T("t")],
                            &[T("perm_id_seq"), T("t")],
                            &[T("perm_pkey"), T("t")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, table_type FROM information_schema.tables WHERE table_name = 'perm' ORDER BY 2;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME), Column("table_type", VARCHAR)],
                        rows: &[
                            &[T("perm"), T("BASE TABLE")],
                            &[T("perm"), T("LOCAL TEMPORARY")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_my_temp_schema() <> 0, pg_is_other_temp_schema(pg_my_temp_schema());",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("pg_is_other_temp_schema", BOOL)],
                        rows: &[
                            &[T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_schemas(false);",
                    expected: Expected::Rows {
                        columns: &[Column("current_schemas", NAME_ARRAY)],
                        rows: &[
                            &[T("{public}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE public.bad (a int);",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "cannot create temporary relation in non-temporary schema", position: 19, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE bad (a int) ON COMMIT DROP;",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "ON COMMIT can only be used on temporary tables", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE perm;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM perm;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE perm;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "temporary tables in transactions",
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE t (a int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "t" does not exist"#, position: 15, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE d (a int) ON COMMIT DELETE ROWS;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM d;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM d;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE dr (a int) ON COMMIT DROP;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dr VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dr;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dr;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "dr" does not exist"#, position: 15, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE c AS SELECT 1 AS one;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("one", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP VIEW v AS SELECT one + 1 AS two FROM c;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v;",
                    expected: Expected::Rows {
                        columns: &[Column("two", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("one", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE TRANSACTION 'p';",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot PREPARE a transaction that has operated on temporary objects", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE TRANSACTION 'p';",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "prepared transactions are disabled", hint: r#"Set "max_prepared_transactions" to a nonzero value."#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT PREPARED 'p';",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"prepared transaction with identifier "p" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD TEMP;",
                    expected: Expected::Tag("DISCARD TEMP"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM c;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "c" does not exist"#, position: 15, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "temporary functions and types",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION public.whoami() RETURNS text AS $$ SELECT 'public'::text $$ LANGUAGE sql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION pg_temp.whoami() RETURNS text AS $$ SELECT 'temp'::text $$ LANGUAGE sql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT whoami();",
                    expected: Expected::Rows {
                        columns: &[Column("whoami", TEXT)],
                        rows: &[
                            &[T("public")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_temp.whoami();",
                    expected: Expected::Rows {
                        columns: &[Column("whoami", TEXT)],
                        rows: &[
                            &[T("temp")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = pg_temp, public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT whoami();",
                    expected: Expected::Rows {
                        columns: &[Column("whoami", TEXT)],
                        rows: &[
                            &[T("public")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN pg_temp.nonempty AS text CHECK (VALUE <> '');",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nonempty('');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function nonempty(unknown) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_temp.nonempty('a');",
                    expected: Expected::Rows {
                        columns: &[Column("nonempty", TEXT)],
                        rows: &[
                            &[T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'b'::nonempty;",
                    expected: Expected::Rows {
                        columns: &[Column("nonempty", TEXT)],
                        rows: &[
                            &[T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT int4('5'), float8('1.5'), date('2020-01-01');",
                    expected: Expected::Rows {
                        columns: &[Column("int4", INT4), Column("float8", FLOAT8), Column("date", DATE)],
                        rows: &[
                            &[T("5"), T("1.5"), T("2020-01-01")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET search_path;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION public.whoami();",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_identity_kinds() {
    run_scripts(&[
        ScriptTest {
            name: "GENERATED ALWAYS and BY DEFAULT identity columns",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ia (a BIGINT NOT NULL PRIMARY KEY GENERATED ALWAYS AS IDENTITY, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ib (a INT GENERATED BY DEFAULT AS IDENTITY, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ia (b) VALUES ('x') RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ia (a, b) VALUES (5, 'y');",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: r#"cannot insert a non-DEFAULT value into column "a""#, detail: r#"Column "a" is an identity column defined as GENERATED ALWAYS."#, hint: "Use OVERRIDING SYSTEM VALUE to override.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ia (a, b) OVERRIDING SYSTEM VALUE VALUES (5, 'y') RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ia (a, b) OVERRIDING USER VALUE VALUES (7, 'z') RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ia VALUES (DEFAULT, 'w') RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ia SELECT 9, 'q';",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: r#"cannot insert a non-DEFAULT value into column "a""#, detail: r#"Column "a" is an identity column defined as GENERATED ALWAYS."#, hint: "Use OVERRIDING SYSTEM VALUE to override.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ia SET a = 10 WHERE b = 'x';",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: r#"column "a" can only be updated to DEFAULT"#, detail: r#"Column "a" is an identity column defined as GENERATED ALWAYS."#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ia SET a = DEFAULT WHERE b = 'x' RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ib (a, b) VALUES (5, 'y') RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ib (a, b) OVERRIDING USER VALUE VALUES (5, 'y') RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attidentity FROM pg_attribute WHERE attrelid IN ('ia'::regclass, 'ib'::regclass) AND attnum > 0 ORDER BY attrelid::regclass::text, attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attidentity", CHAR)],
                        rows: &[
                            &[T("a"), T("a")],
                            &[T("b"), T("")],
                            &[T("a"), T("d")],
                            &[T("b"), T("")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ib ALTER COLUMN b SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ic (a INT NOT NULL, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ic ALTER COLUMN a ADD GENERATED ALWAYS AS IDENTITY;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ic VALUES (1, 'x');",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: r#"cannot insert a non-DEFAULT value into column "a""#, detail: r#"Column "a" is an identity column defined as GENERATED ALWAYS."#, hint: "Use OVERRIDING SYSTEM VALUE to override.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_temporary_objects() {
    run_scripts(&[
        ScriptTest {
            name: "temporary tables, their indexes, and temporary sequences live in the session's temporary schema",
            set_up_script: &[
                "CREATE TEMP TABLE t (a INT PRIMARY KEY, b TEXT);",
                "INSERT INTO t VALUES (1, 'one'), (2, 'two');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n.nspname, c.relname, c.relpersistence FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.relname IN ('t', 't_pkey') ORDER BY 2;",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME), Column("relname", NAME), Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("pg_temp_0"), T("t"), T("t")],
                            &[T("pg_temp_0"), T("t_pkey"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nspname FROM pg_namespace WHERE oid = pg_my_temp_schema();",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME)],
                        rows: &[
                            &[T("pg_temp_0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP SEQUENCE s;",
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('s'), nextval('s');",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8), Column("nextval", INT8)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE p (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p SELECT a FROM t;",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM p;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE r (x INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname = 'r';",
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
                    query: "CREATE TEMP TABLE r (x INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "temporary tables keep large values, indexes, statistics, and ON COMMIT actions, and views over them are temporary",
            set_up_script: &[
                "CREATE TEMP TABLE big (id INT PRIMARY KEY, v TEXT);",
                "INSERT INTO big SELECT g, repeat('x', 3000) FROM generate_series(1, 20) g;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(length(v)) FROM big;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("20"), T("60000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX big_v ON big (left(v, 5));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE big;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, reltuples FROM pg_class WHERE relname IN ('big', 'big_pkey', 'big_v') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("big"), T("20")],
                            &[T("big_pkey"), T("20")],
                            &[T("big_v"), T("20")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE big;",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM big;",
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
                    query: "CREATE TEMP VIEW tv AS SELECT 1 AS one;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tv;",
                    expected: Expected::Rows {
                        columns: &[Column("one", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW pv AS SELECT * FROM big;",
                    expected: Expected::Tag("CREATE VIEW"),
                    notices: &[Diagnostic { code: "00000", message: r#"view "pv" will be a temporary view"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpersistence FROM pg_class WHERE relname = 'pv';",
                    expected: Expected::Rows {
                        columns: &[Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE oc_del (a INT) ON COMMIT DELETE ROWS;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc_del VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM oc_del;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM oc_del;",
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE oc_drop (a INT) ON COMMIT DROP;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc_drop VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname = 'oc_drop';",
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
                    query: "CREATE TEMP TABLE oc_keep (a INT) ON COMMIT PRESERVE ROWS;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc_keep VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM oc_keep;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE perm (a INT) ON COMMIT DELETE ROWS;",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "ON COMMIT can only be used on temporary tables", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD TEMP;",
                    expected: Expected::Tag("DISCARD TEMP"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname IN ('big', 'oc_del', 'oc_keep', 'tv', 'pv');",
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
                    query: "CREATE TEMP TABLE again (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname FROM pg_class WHERE relname = 'again';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME)],
                        rows: &[
                            &[T("again")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "temporary tables' ON COMMIT actions follow savepoints, DISCARD TEMP rolls back, and PREPARE TRANSACTION refuses them",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE sd (a INT) ON COMMIT DELETE ROWS;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s1;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE sx (a INT) ON COMMIT DROP;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT s1;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sd VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sd;",
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
                    query: "SELECT count(*) FROM pg_class WHERE relname = 'sx';",
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s2;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE sd;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT s2;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sd VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sd;",
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s3;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE sy (a INT) ON COMMIT DROP;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RELEASE SAVEPOINT s3;",
                    expected: Expected::Tag("RELEASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname = 'sy';",
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
                    query: "CREATE TEMP TABLE keep (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keep VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DISCARD TEMP;",
                    expected: Expected::Tag("DISCARD TEMP"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname = 'keep';",
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
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keep;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keep;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE TRANSACTION 'p1';",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot PREPARE a transaction that has operated on temporary objects", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE ref (a INT PRIMARY KEY) ON COMMIT DELETE ROWS;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE child (a INT REFERENCES ref);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "unsupported ON COMMIT and foreign key combination", detail: r#"Table "child" references "ref", but they do not have the same ON COMMIT setting."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ref VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_is_other_temp_schema(pg_my_temp_schema());",
                    expected: Expected::Rows {
                        columns: &[Column("pg_is_other_temp_schema", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_is_other_temp_schema(oid) FROM pg_namespace WHERE nspname = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_is_other_temp_schema", BOOL)],
                        rows: &[
                            &[T("f")],
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
fn test_temporary_objects_of_other_sessions() {
    run_scripts(&[
        ScriptTest {
            name: "another session sees a session's temporary schema and tables, may lock and drop them, but not read, change, or empty them",
            set_up_script: &[
                "CREATE TEMP TABLE tvis (a INT)",
                "INSERT INTO tvis VALUES (1)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM pg_class WHERE relname = 'tvis'",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM pg_namespace WHERE nspname IN ('pg_temp_0', 'pg_toast_temp_0')",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_is_other_temp_schema(relnamespace) AS other FROM pg_class WHERE relname = 'tvis'",
                    expected: Expected::Rows {
                        columns: &[Column("other", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM pg_temp_0.tvis",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot access temporary tables of other sessions", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO pg_temp_0.tvis VALUES (2)",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot access temporary tables of other sessions", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ TRUNCATE pg_temp_0.tvis",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot truncate temporary tables of other sessions", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ LOCK TABLE pg_temp_0.tvis",
                    expected: Expected::Tag("LOCK TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM tvis",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DROP TABLE pg_temp_0.tvis",
                    expected: Expected::Tag("DROP TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pg_class WHERE relname = 'tvis'",
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
                    query: "/* client A */ CREATE TEMP TABLE tvis2 AS SELECT 1 AS a",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM pg_temp_0.tvis2",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot access temporary tables of other sessions", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DROP TABLE pg_temp_0.tvis2",
                    expected: Expected::Tag("DROP TABLE"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "another session's DROP of a temporary table waits for the session's lock, and a rolled-back DROP keeps the table",
            set_up_script: &[
                "CREATE TEMP TABLE tlock (a INT)",
                "INSERT INTO tlock VALUES (1)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM tlock",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DROP TABLE pg_temp_0.tlock",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT 1",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM pg_class WHERE relname = 'tlock'",
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
                    query: "/* client A */ CREATE TEMP TABLE tlock2 (a INT)",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO tlock2 VALUES (7)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ DROP TABLE pg_temp_0.tlock2",
                    expected: Expected::Tag("DROP TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SET lock_timeout = '200ms'",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM tlock2",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "canceling statement due to lock timeout", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ RESET lock_timeout",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM tlock2",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a session's commits that touch temporary tables fail after another session dropped its ON COMMIT DELETE ROWS table",
            set_up_script: &[
                "CREATE TEMP TABLE tdel (a INT) ON COMMIT DELETE ROWS",
                "CREATE TEMP TABLE tother (a INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ DROP TABLE pg_temp_0.tdel",
                    expected: Expected::Tag("DROP TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO tother VALUES (1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "could not open relation with OID", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM tother",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "could not open relation with OID", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT 1 AS a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
    ]);
}

#[test]
fn test_unlogged_tables() {
    run_scripts(&[
        ScriptTest {
            name: "unlogged tables, their indexes, and unlogged sequences",
            set_up_script: &[
                "CREATE UNLOGGED TABLE ul (a INT PRIMARY KEY, b TEXT)",
                "CREATE INDEX ul_b ON ul (b)",
                "CREATE UNLOGGED SEQUENCE uls START 5",
                "CREATE TABLE lg (a INT PRIMARY KEY)",
                "INSERT INTO ul VALUES (1, 'one'), (2, 'two')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT relname, relpersistence FROM pg_class WHERE relname IN ('ul', 'ul_b', 'ul_pkey', 'uls', 'lg', 'lg_pkey') ORDER BY relname",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("lg"), T("p")],
                            &[T("lg_pkey"), T("p")],
                            &[T("ul"), T("u")],
                            &[T("ul_b"), T("u")],
                            &[T("ul_pkey"), T("u")],
                            &[T("uls"), T("u")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ul ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('uls')",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('uls')",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ul SET b = 'uno' WHERE a = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ul WHERE a = 2",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ul ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("uno")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE ul",
                    expected: Expected::Tag("TRUNCATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ul",
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
                    query: "CREATE UNLOGGED TABLE ulp (a INT) PARTITION BY RANGE (a)",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "partitioned tables cannot be unlogged", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fk_lg (a INT REFERENCES ul (a))",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "constraints on permanent tables may reference only permanent tables", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNLOGGED TABLE fk_ul (a INT REFERENCES lg (a))",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ul SET LOGGED",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpersistence FROM pg_class WHERE relname = 'ul'",
                    expected: Expected::Rows {
                        columns: &[Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("p")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE lg SET UNLOGGED",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpersistence FROM pg_class WHERE relname = 'lg'",
                    expected: Expected::Rows {
                        columns: &[Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("u")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE ul CASCADE",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SEQUENCE uls",
                    expected: Expected::Tag("DROP SEQUENCE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_set_logged_and_unlogged() {
    run_scripts(&[
        ScriptTest {
            name: "SET LOGGED and SET UNLOGGED change a table's persistence with its indexes and owned sequences, and a sequence's",
            set_up_script: &[
                "CREATE TABLE p1 (a INT PRIMARY KEY)",
                "CREATE UNLOGGED TABLE u1 (a INT PRIMARY KEY, b SERIAL)",
                "CREATE UNLOGGED TABLE u2 (a INT REFERENCES p1 (a))",
                "CREATE UNLOGGED TABLE u3 (a INT PRIMARY KEY)",
                "CREATE UNLOGGED TABLE u4 (a INT REFERENCES u3 (a))",
                "CREATE TEMP TABLE t1 (a INT)",
                "CREATE VIEW v1 AS SELECT 1 AS a",
                "CREATE SEQUENCE s1",
                "INSERT INTO p1 VALUES (1), (2)",
                "INSERT INTO u1 (a) VALUES (1), (2), (3)",
                "INSERT INTO u3 VALUES (1)",
                "INSERT INTO u4 VALUES (1)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE p1 SET UNLOGGED",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u4 SET LOGGED",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: r#"could not change table "u4" to logged because it references unlogged table "u3""#, schema: "public", table: "u4", constraint: "u4_a_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u3 SET LOGGED",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 SET LOGGED",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: r#"cannot change logged status of table "t1" because it is temporary"#, schema: "pg_temp_0", table: "t1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 SET UNLOGGED",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: r#"cannot change logged status of table "t1" because it is temporary"#, schema: "pg_temp_0", table: "t1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u1 SET LOGGED, SET UNLOGGED",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot change persistence setting twice", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE v1 SET LOGGED",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"ALTER action SET LOGGED cannot be performed on relation "v1""#, detail: "This operation is not supported for views.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u1 SET LOGGED",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relpersistence FROM pg_class WHERE relname IN ('u1', 'u1_pkey', 'u1_b_seq') ORDER BY relname",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("u1"), T("p")],
                            &[T("u1_b_seq"), T("p")],
                            &[T("u1_pkey"), T("p")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM u1 ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('u1_b_seq')",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u1 SET UNLOGGED",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relpersistence FROM pg_class WHERE relname IN ('u1', 'u1_pkey', 'u1_b_seq') ORDER BY relname",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("u1"), T("u")],
                            &[T("u1_b_seq"), T("u")],
                            &[T("u1_pkey"), T("u")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('u1_b_seq')",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SEQUENCE s1 SET UNLOGGED",
                    expected: Expected::Tag("ALTER SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpersistence FROM pg_class WHERE relname = 's1'",
                    expected: Expected::Rows {
                        columns: &[Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("u")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('s1')",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE s1 SET LOGGED",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpersistence FROM pg_class WHERE relname = 's1'",
                    expected: Expected::Rows {
                        columns: &[Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("p")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('s1')",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u3 SET LOGGED",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpersistence FROM pg_class WHERE relname = 'u3'",
                    expected: Expected::Rows {
                        columns: &[Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("p")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u3",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT sp",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u1 SET LOGGED",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT sp",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpersistence FROM pg_class WHERE relname = 'u1'",
                    expected: Expected::Rows {
                        columns: &[Column("relpersistence", CHAR)],
                        rows: &[
                            &[T("u")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO u1 (a) VALUES (4)",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM u1 ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p1 SET LOGGED",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_generated_columns() {
    run_scripts(&[
        ScriptTest {
            name: "virtual generated columns and the expressions generated columns cannot have",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE v (a INT PRIMARY KEY, b INT GENERATED ALWAYS AS (a * 3), c TEXT GENERATED ALWAYS AS ('n' || a) VIRTUAL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v (a, b) VALUES (3, 4);",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: "cannot insert a non-DEFAULT value into column \"b\"", detail: "Column \"b\" is a generated column.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v (a, b) VALUES (3, DEFAULT) RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("3"), T("9"), T("n3")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE v SET a = a + 10 WHERE a = 1 RETURNING b, c;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("33"), T("n11")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE v SET c = 'x';",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: "column \"c\" can only be updated to DEFAULT", detail: "Column \"c\" is a generated column.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("2"), T("6"), T("n2")],
                            &[T("3"), T("9"), T("n3")],
                            &[T("11"), T("33"), T("n11")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM v WHERE b > 6 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("11")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attgenerated FROM pg_attribute WHERE attrelid = 'v'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attgenerated", CHAR)],
                        rows: &[
                            &[T("a"), T("")],
                            &[T("b"), T("v")],
                            &[T("c"), T("v")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE v ADD COLUMN d INT GENERATED ALWAYS AS (a + 1) VIRTUAL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, d FROM v ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("2"), T("3")],
                            &[T("3"), T("4")],
                            &[T("11"), T("12")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION gen_f(x INT) RETURNS INT IMMUTABLE LANGUAGE SQL AS 'SELECT x + 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE v2 (a INT, b INT GENERATED ALWAYS AS (gen_f(a)) VIRTUAL);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "generation expression uses user-defined function", position: 52, detail: "Virtual generated columns that make use of user-defined functions are not yet supported.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE s2 (a INT, b INT GENERATED ALWAYS AS (gen_f(a)) STORED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO s2 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE v3 (a INT, b INT GENERATED ALWAYS AS (a + 1) GENERATED ALWAYS AS (a + 2));",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "multiple generation clauses specified for column \"b\" of table \"v3\"", position: 59, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE v4 (a INT, b INT GENERATED ALWAYS AS IDENTITY GENERATED ALWAYS AS (a + 1) STORED);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "both identity and generation expression specified for column \"b\" of table \"v4\"", position: 60, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE gen_t AS (a INT, b INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE v5 OF gen_t (b WITH OPTIONS GENERATED ALWAYS AS (a + 1) STORED);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "generated columns are not supported on typed tables", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE v6 (a INT, b INT GENERATED ALWAYS AS (random()::INT) VIRTUAL);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "generation expression is not immutable", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "generated columns added to a table with rows",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE r (a INT PRIMARY KEY, b TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r VALUES (1, 'one'), (2, 'two'), (3, NULL);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r ADD COLUMN c INT GENERATED ALWAYS AS (a * 10) STORED;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r ADD COLUMN d TEXT GENERATED ALWAYS AS (upper(b)) STORED;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", INT4), Column("d", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("10"), T("ONE")],
                            &[T("2"), T("two"), T("20"), T("TWO")],
                            &[T("3"), Null, T("30"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE r SET b = 'drei' WHERE a = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, d FROM r WHERE a = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("d", TEXT)],
                        rows: &[
                            &[T("3"), T("DREI")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r WHERE c = 20;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r ADD COLUMN e INT GENERATED ALWAYS AS (a / (a - 2)) STORED;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r ADD COLUMN e INT GENERATED ALWAYS AS (a - 2) STORED CHECK (e > 0);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "check constraint \"r_e_check\" of relation \"r\" is violated by some row", schema: "public", table: "r", constraint: "r_e_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r ADD COLUMN e TEXT GENERATED ALWAYS AS (b) STORED NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname FROM pg_attribute WHERE attrelid = 'r'::regclass AND attnum > 0 AND NOT attisdropped ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME)],
                        rows: &[
                            &[T("a")],
                            &[T("b")],
                            &[T("c")],
                            &[T("d")],
                            &[T("e")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r ADD COLUMN f INT GENERATED ALWAYS AS (a + 1) STORED, ADD COLUMN g INT DEFAULT 7;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", INT4), Column("d", TEXT), Column("e", TEXT), Column("f", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("one"), T("10"), T("ONE"), T("one"), T("2"), T("7")],
                            &[T("2"), T("two"), T("20"), T("TWO"), T("two"), T("3"), T("7")],
                            &[T("3"), T("drei"), T("30"), T("DREI"), T("drei"), T("4"), T("7")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX r_c ON r (c);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r (a) VALUES (4);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"e\" of relation \"r\" violates not-null constraint", detail: "Failing row contains (4, null, 40, null, null, 5, 7).", schema: "public", table: "r", column: "e", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, c, f, g FROM r ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("c", INT4), Column("f", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("2"), T("7")],
                            &[T("2"), T("20"), T("3"), T("7")],
                            &[T("3"), T("30"), T("4"), T("7")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "generated columns' constraints, types, expressions, partitions, and triggers",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE vn (a INT, b INT GENERATED ALWAYS AS (nullif(a, 0)) VIRTUAL NOT NULL, CONSTRAINT vn_check CHECK (b < 100));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO vn VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO vn VALUES (0);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"b\" of relation \"vn\" violates not-null constraint", detail: "Failing row contains (0, virtual).", schema: "public", table: "vn", column: "b", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO vn VALUES (200);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"vn\" violates check constraint \"vn_check\"", detail: "Failing row contains (200, virtual).", schema: "public", table: "vn", constraint: "vn_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE vn SET a = 0;",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"b\" of relation \"vn\" violates not-null constraint", detail: "Failing row contains (0, virtual).", schema: "public", table: "vn", column: "b", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE vn ADD COLUMN c INT GENERATED ALWAYS AS (nullif(a, 1)) VIRTUAL NOT NULL;",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "column \"c\" of relation \"vn\" contains null values", schema: "public", table: "vn", column: "c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE vn ADD COLUMN c INT GENERATED ALWAYS AS (a + 1) VIRTUAL NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE vw (a INT, b INT GENERATED ALWAYS AS (a * 2) VIRTUAL, CONSTRAINT vw_whole CHECK (vw IS NOT NULL));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO vw VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO vw VALUES (NULL);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"vw\" violates check constraint \"vw_whole\"", detail: "Failing row contains (null, virtual).", schema: "public", table: "vw", constraint: "vw_whole", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN gen_d AS INT CHECK (VALUE < 10);",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sd (a INT PRIMARY KEY, b gen_d GENERATED ALWAYS AS (a * 2) STORED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sd (a) VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO sd (a) VALUES (6);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "value for domain gen_d violates check constraint \"gen_d_check\"", schema: "public", data_type: "gen_d", constraint: "gen_d_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE sd SET a = 5;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "value for domain gen_d violates check constraint \"gen_d_check\"", schema: "public", data_type: "gen_d", constraint: "gen_d_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE vd (a INT, b gen_d GENERATED ALWAYS AS (a * 2) VIRTUAL);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "virtual generated column \"b\" cannot have a domain type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE gen_pair AS (x INT, y INT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE vt (a INT, b gen_pair GENERATED ALWAYS AS (ROW(a, a)) VIRTUAL);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "virtual generated column \"b\" cannot have a user-defined type", detail: "Virtual generated columns that make use of user-defined types are not yet supported.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE sd ADD COLUMN c gen_d GENERATED ALWAYS AS (a) VIRTUAL;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "virtual generated column \"c\" cannot have a domain type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ve (a INT, b INT GENERATED ALWAYS AS (a * 2) VIRTUAL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ve VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ve ALTER COLUMN b SET EXPRESSION AS (a * 10);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ve ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ve ALTER COLUMN a SET EXPRESSION AS (1);",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "column \"a\" of relation \"ve\" is not a generated column", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ve ALTER COLUMN b DROP EXPRESSION;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "ALTER TABLE / DROP EXPRESSION is not supported for virtual generated columns", detail: "Column \"b\" of relation \"ve\" is a virtual generated column.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ve ALTER COLUMN a DROP EXPRESSION;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "column \"a\" of relation \"ve\" is not a generated column", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ve ALTER COLUMN a DROP EXPRESSION IF EXISTS;",
                    notices: &[Diagnostic { code: "00000", message: "column \"a\" of relation \"ve\" is not a generated column, skipping", ..N }],
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE vn ALTER COLUMN b SET EXPRESSION AS (a);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "ALTER TABLE / SET EXPRESSION is not supported for virtual generated columns in tables with check constraints", detail: "Column \"b\" of relation \"vn\" is a virtual generated column.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE se (a INT, b INT GENERATED ALWAYS AS (a * 2) STORED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO se VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE se ALTER COLUMN b DROP EXPRESSION;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO se VALUES (3, 33);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM se ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("4")],
                            &[T("3"), T("33")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attgenerated, atthasdef FROM pg_attribute WHERE attrelid = 'se'::regclass AND attname = 'b';",
                    expected: Expected::Rows {
                        columns: &[Column("attgenerated", CHAR), Column("atthasdef", BOOL)],
                        rows: &[
                            &[T(""), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sp (a INT, b INT GENERATED ALWAYS AS (a * 2) STORED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE sc () INHERITS (sp);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ONLY sp ALTER COLUMN b DROP EXPRESSION;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "ALTER TABLE / DROP EXPRESSION must be applied to child tables too", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE sc ALTER COLUMN b DROP EXPRESSION;",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "cannot drop generation expression from inherited column", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE sp ALTER COLUMN b DROP EXPRESSION;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ss (a INT, b INT GENERATED ALWAYS AS (a * 2) STORED);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ss VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ss ALTER COLUMN b SET EXPRESSION AS (a * 3);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ss;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pt (a INT, b INT GENERATED ALWAYS AS (a * 2) STORED) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pt2 PARTITION OF pt (b GENERATED ALWAYS AS (a * 3) STORED) FOR VALUES FROM (10) TO (20);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt (a) VALUES (1), (11);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET a = a + 1;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM pt ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("pt1"), T("2"), T("4")],
                            &[T("pt2"), T("12"), T("36")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tg (a INT PRIMARY KEY, b INT GENERATED ALWAYS AS (a * 2) VIRTUAL, c INT GENERATED ALWAYS AS (a * 3) STORED, d INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION tg_f() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE '% fired', TG_NAME; RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tg_b BEFORE UPDATE OF b ON tg FOR EACH ROW EXECUTE FUNCTION tg_f();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tg_c AFTER UPDATE OF c ON tg FOR EACH ROW EXECUTE FUNCTION tg_f();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tg_s BEFORE UPDATE OF c ON tg FOR EACH STATEMENT EXECUTE FUNCTION tg_f();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tg (a, d) VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tg SET d = 2;",
                    notices: &[Diagnostic { code: "00000", message: "tg_s fired", ..N }, Diagnostic { code: "00000", message: "tg_b fired", ..N }, Diagnostic { code: "00000", message: "tg_c fired", ..N }],
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tg SET a = 2;",
                    notices: &[Diagnostic { code: "00000", message: "tg_s fired", ..N }, Diagnostic { code: "00000", message: "tg_b fired", ..N }, Diagnostic { code: "00000", message: "tg_c fired", ..N }],
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tg;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("2"), T("4"), T("6"), T("2")],
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
fn test_create_table_like() {
    run_scripts(&[
        ScriptTest {
            name: "LIKE copies columns, not-null constraints, and with INCLUDING ALL the defaults, generated columns, identity, storage, compression, constraints, indexes, statistics, and comments",
            set_up_script: &[
                "CREATE TABLE src (id INT PRIMARY KEY, a TEXT COMPRESSION lz4 NOT NULL DEFAULT 'x', b INT CHECK (b > 0), c INT GENERATED ALWAYS AS (b * 2) STORED, d INT GENERATED BY DEFAULT AS IDENTITY (START 10 INCREMENT 5), e VARCHAR(10) UNIQUE);",
                "CREATE INDEX src_a ON src (lower(a)) WHERE b > 1;",
                "CREATE STATISTICS src_st (ndistinct) ON a, b FROM src;",
                "ALTER TABLE src ALTER COLUMN a SET STORAGE EXTERNAL;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COMMENT ON COLUMN src.a IS 'column a';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON CONSTRAINT src_b_check ON src IS 'b positive';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON INDEX src_a IS 'lower a';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON STATISTICS src_st IS 'stats';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (LIKE src);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, format_type(atttypid, atttypmod), attnotnull, atthasdef, attidentity, attgenerated, attstorage, attcompression FROM pg_attribute WHERE attrelid = 't1'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("atthasdef", BOOL), Column("attidentity", CHAR), Column("attgenerated", CHAR), Column("attstorage", CHAR), Column("attcompression", CHAR)],
                        rows: &[
                            &[T("id"), T("integer"), T("t"), T("f"), T(""), T(""), T("p"), T("")],
                            &[T("a"), T("text"), T("t"), T("f"), T(""), T(""), T("x"), T("")],
                            &[T("b"), T("integer"), T("f"), T("f"), T(""), T(""), T("p"), T("")],
                            &[T("c"), T("integer"), T("f"), T("f"), T(""), T(""), T("p"), T("")],
                            &[T("d"), T("integer"), T("t"), T("f"), T(""), T(""), T("p"), T("")],
                            &[T("e"), T("character varying(10)"), T("f"), T("f"), T(""), T(""), T("x"), T("")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 't1'::regclass ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("src_a_not_null"), T("NOT NULL a")],
                            &[T("src_d_not_null"), T("NOT NULL d")],
                            &[T("src_id_not_null"), T("NOT NULL id")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (LIKE src INCLUDING ALL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, format_type(atttypid, atttypmod), attnotnull, pg_get_expr(adbin, adrelid), attidentity, attgenerated, attstorage, attcompression FROM pg_attribute LEFT JOIN pg_attrdef ON adrelid = attrelid AND adnum = attnum WHERE attrelid = 't2'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT), Column("attnotnull", BOOL), Column("pg_get_expr", TEXT), Column("attidentity", CHAR), Column("attgenerated", CHAR), Column("attstorage", CHAR), Column("attcompression", CHAR)],
                        rows: &[
                            &[T("id"), T("integer"), T("t"), Null, T(""), T(""), T("p"), T("")],
                            &[T("a"), T("text"), T("t"), T("'x'::text"), T(""), T(""), T("e"), T("l")],
                            &[T("b"), T("integer"), T("f"), Null, T(""), T(""), T("p"), T("")],
                            &[T("c"), T("integer"), T("f"), T("(b * 2)"), T(""), T("s"), T("p"), T("")],
                            &[T("d"), T("integer"), T("t"), Null, T("d"), T(""), T("p"), T("")],
                            &[T("e"), T("character varying(10)"), T("f"), Null, T(""), T(""), T("x"), T("")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid), obj_description(oid, 'pg_constraint') FROM pg_constraint WHERE conrelid = 't2'::regclass ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT), Column("obj_description", TEXT)],
                        rows: &[
                            &[T("src_a_not_null"), T("NOT NULL a"), Null],
                            &[T("src_b_check"), T("CHECK ((b > 0))"), T("b positive")],
                            &[T("src_d_not_null"), T("NOT NULL d"), Null],
                            &[T("src_id_not_null"), T("NOT NULL id"), Null],
                            &[T("t2_e_key"), T("UNIQUE (e)"), Null],
                            &[T("t2_pkey"), T("PRIMARY KEY (id)"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, pg_get_indexdef(i.indexrelid), obj_description(c.oid, 'pg_class') FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid = 't2'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("pg_get_indexdef", TEXT), Column("obj_description", TEXT)],
                        rows: &[
                            &[T("t2_e_key"), T("CREATE UNIQUE INDEX t2_e_key ON public.t2 USING btree (e)"), Null],
                            &[T("t2_lower_idx"), T("CREATE INDEX t2_lower_idx ON public.t2 USING btree (lower(a)) WHERE (b > 1)"), T("lower a")],
                            &[T("t2_pkey"), T("CREATE UNIQUE INDEX t2_pkey ON public.t2 USING btree (id)"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stxname, pg_get_statisticsobjdef(oid), obj_description(oid, 'pg_statistic_ext') FROM pg_statistic_ext WHERE stxrelid = 't2'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("stxname", NAME), Column("pg_get_statisticsobjdef", TEXT), Column("obj_description", TEXT)],
                        rows: &[
                            &[T("t2_a_b_stat"), T("CREATE STATISTICS public.t2_a_b_stat (ndistinct) ON a, b FROM t2"), T("stats")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT col_description('t2'::regclass, 2);",
                    expected: Expected::Rows {
                        columns: &[Column("col_description", TEXT)],
                        rows: &[
                            &[T("column a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT seqstart, seqincrement FROM pg_sequence WHERE seqrelid = pg_get_serial_sequence('t2', 'd')::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("seqstart", INT8), Column("seqincrement", INT8)],
                        rows: &[
                            &[T("10"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 (id, a, b) VALUES (1, 'q', 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 (id, b) VALUES (2, 4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", TEXT), Column("b", INT4), Column("c", INT4), Column("d", INT4), Column("e", VARCHAR)],
                        rows: &[
                            &[T("1"), T("q"), T("3"), T("6"), T("10"), Null],
                            &[T("2"), T("x"), T("4"), T("8"), T("15"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 (id, b) VALUES (3, -1);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t2\" violates check constraint \"src_b_check\"", detail: "Failing row contains (3, x, -1, -2, 20, null).", schema: "public", table: "t2", constraint: "src_b_check", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "LIKE places columns where it stands, keeps collations, skips dropped columns, and refuses what Postgres refuses",
            set_up_script: &[
                "CREATE TABLE p (a INT NOT NULL, b TEXT COLLATE \"C\", z INT, CONSTRAINT p_chk CHECK (a < 100) NO INHERIT);",
                "ALTER TABLE p DROP COLUMN z;",
                "ALTER TABLE p ADD COLUMN c INT DEFAULT 7;",
                "CREATE TABLE w (a INT, CHECK ((w.*) IS NOT NULL));",
                "CREATE TYPE ct AS (p INT, q TEXT);",
                "CREATE TABLE src (id INT PRIMARY KEY, v TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE l1 (x INT, LIKE p INCLUDING ALL, y INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attnum, attname, format_type(atttypid, atttypmod), attcollation::regcollation FROM pg_attribute WHERE attrelid = 'l1'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attnum", INT2), Column("attname", NAME), Column("format_type", TEXT), Column("attcollation", REGCOLLATION)],
                        rows: &[
                            &[T("1"), T("x"), T("integer"), T("-")],
                            &[T("2"), T("a"), T("integer"), T("-")],
                            &[T("3"), T("b"), T("text"), T("\"C\"")],
                            &[T("4"), T("c"), T("integer"), T("-")],
                            &[T("5"), T("y"), T("integer"), T("-")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid), connoinherit FROM pg_constraint WHERE conrelid = 'l1'::regclass ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT), Column("connoinherit", BOOL)],
                        rows: &[
                            &[T("p_a_not_null"), T("NOT NULL a"), T("f")],
                            &[T("p_chk"), T("CHECK ((a < 100)) NO INHERIT"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l2 (LIKE p, LIKE p);",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"a\" specified more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l3 (a INT, LIKE p);",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"a\" specified more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l4 (LIKE w INCLUDING CONSTRAINTS);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot convert whole-row table reference", detail: "Constraint \"w_check\" contains a whole-row reference to table \"w\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l5 (LIKE nosuch);",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"nosuch\" does not exist", position: 23, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX src_v ON src (v);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l6 (LIKE src_v);",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "relation \"src_v\" is invalid in LIKE clause", position: 23, detail: "This operation is not supported for indexes.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l7 (LIKE ct);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'l7'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT)],
                        rows: &[
                            &[T("p"), T("integer")],
                            &[T("q"), T("text")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l8 (LIKE src INCLUDING INDEXES, PRIMARY KEY (v));",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "multiple primary keys for table \"l8\" are not allowed", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l9 (id INT PRIMARY KEY, LIKE src INCLUDING INDEXES);",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"id\" specified more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l10 (LIKE src INCLUDING ALL EXCLUDING INDEXES);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_index WHERE indrelid = 'l10'::regclass;",
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
            name: "LIKE reports its commands to event triggers and needs SELECT on the table it copies",
            set_up_script: &[
                "CREATE FUNCTION ec() RETURNS event_trigger LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN FOR r IN SELECT * FROM pg_event_trigger_ddl_commands() LOOP RAISE NOTICE 'cmd % % %', r.command_tag, r.object_type, r.object_identity; END LOOP; END $$;",
                "CREATE TABLE src (id INT PRIMARY KEY, a INT DEFAULT 1 CHECK (a > 0), b TEXT);",
                "CREATE INDEX src_b ON src (b);",
                "CREATE STATISTICS src_st ON id, a FROM src;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER ect ON ddl_command_end EXECUTE FUNCTION ec();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l1 (LIKE src INCLUDING ALL);",
                    notices: &[Diagnostic { code: "00000", message: "cmd CREATE TABLE table public.l1", ..N }, Diagnostic { code: "00000", message: "cmd ALTER TABLE table public.l1", ..N }, Diagnostic { code: "00000", message: "cmd CREATE INDEX index public.l1_pkey", ..N }, Diagnostic { code: "00000", message: "cmd CREATE INDEX index public.l1_b_idx", ..N }, Diagnostic { code: "00000", message: "cmd CREATE STATISTICS statistics object public.l1_id_a_stat", ..N }],
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER ect;",
                    expected: Expected::Tag("DROP EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE like_user;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO like_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE like_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l2 (LIKE src);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table src", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON src TO like_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE like_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE l2 (LIKE src INCLUDING ALL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableowner FROM pg_tables WHERE tablename = 'l2';",
                    expected: Expected::Rows {
                        columns: &[Column("tableowner", NAME)],
                        rows: &[
                            &[T("like_user")],
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
fn test_typed_tables() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE TABLE OF makes typed tables of composite types, which depend on their type",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TYPE person_type AS (id INT, name TEXT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE persons OF person_type;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE IF NOT EXISTS persons OF person_type;",
                    notices: &[Diagnostic { code: "42P07", message: "relation \"persons\" already exists, skipping", ..N }],
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE persons2 OF person_type (id WITH OPTIONS PRIMARY KEY, UNIQUE (name));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE persons3 OF person_type (PRIMARY KEY (id), name WITH OPTIONS DEFAULT 'none');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE persons4 OF person_type (name WITH OPTIONS NOT NULL, name WITH OPTIONS DEFAULT '');",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"name\" specified more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE persons5 OF person_type (myname WITH OPTIONS NOT NULL);",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"myname\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO persons3 (id) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM persons3;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("none")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, t.typname FROM pg_class c JOIN pg_type t ON t.oid = c.reloftype WHERE c.relname LIKE 'persons%' ORDER BY c.relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("typname", NAME)],
                        rows: &[
                            &[T("persons"), T("person_type")],
                            &[T("persons2"), T("person_type")],
                            &[T("persons3"), T("person_type")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.atttypid::regtype, a.attnotnull FROM pg_attribute a WHERE a.attrelid = 'persons3'::regclass AND a.attnum > 0 ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("atttypid", REGTYPE), Column("attnotnull", BOOL)],
                        rows: &[
                            &[T("id"), T("integer"), T("t")],
                            &[T("name"), T("text"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT d.deptype FROM pg_depend d WHERE d.classid = 'pg_class'::regclass AND d.objid = 'persons'::regclass AND d.refclassid = 'pg_type'::regclass AND d.refobjid = 'person_type'::regtype;",
                    expected: Expected::Rows {
                        columns: &[Column("deptype", CHAR)],
                        rows: &[
                            &[T("n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE persons ADD COLUMN comment TEXT;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot add column to typed table", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE persons DROP COLUMN name;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot drop column from typed table", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE persons RENAME COLUMN id TO num;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot rename column of typed table", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE persons ALTER COLUMN name TYPE VARCHAR;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot alter column type of typed table", position: 34, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE stuff (id INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE persons INHERIT stuff;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "cannot change inheritance of typed table", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE stuff_typed OF stuff;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "type stuff is the row type of another table", detail: "A typed table must use a stand-alone composite type created with CREATE TYPE.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE tt_enum_type AS ENUM ('a');",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tt_enum_typed OF tt_enum_type;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "type tt_enum_type is not a composite type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE nothing_typed OF nothing;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "type \"nothing\" does not exist", position: 31, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TYPE person_type RESTRICT;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop type person_type because other objects depend on it", detail: "table persons depends on type person_type\ntable persons2 depends on type person_type\ntable persons3 depends on type person_type", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TYPE person_type CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to 3 other objects", detail: "drop cascades to table persons\ndrop cascades to table persons2\ndrop cascades to table persons3", ..N }],
                    expected: Expected::Tag("DROP TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname LIKE 'persons%';",
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
fn test_stored_expressions_are_planned_before_they_run() {
    run_scripts(&[
        ScriptTest {
            name: "CHECK constraints, generated columns, trigger conditions, and domain checks with COLLATE",
            set_up_script: &[
                r#"CREATE TABLE chk (t TEXT CHECK (t > 'b' COLLATE "C"));"#,
                r#"CREATE TABLE gen (t TEXT, u TEXT GENERATED ALWAYS AS (t COLLATE "C" || 'x') STORED, v TEXT GENERATED ALWAYS AS (NULLIF(t COLLATE "C", 'z')) VIRTUAL NOT NULL);"#,
                "CREATE TABLE trg (t TEXT);",
                "CREATE FUNCTION trg_note() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'fired for %', NEW.t; RETURN NEW; END $$;",
                r#"CREATE TRIGGER trg_when BEFORE INSERT ON trg FOR EACH ROW WHEN (NEW.t > 'b' COLLATE "C") EXECUTE FUNCTION trg_note();"#,
                "CREATE DOMAIN dom AS TEXT;",
                "CREATE TABLE domt (d dom);",
                "INSERT INTO domt VALUES ('a');",
                "CREATE TABLE parted (t TEXT) PARTITION BY LIST (t);",
                "CREATE TABLE parted_a PARTITION OF parted FOR VALUES IN ('a');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO chk VALUES ('c');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO chk VALUES ('a');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "chk" violates check constraint "chk_t_check""#, detail: "Failing row contains (a).", schema: "public", table: "chk", constraint: "chk_t_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE chk SET t = 'B';",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "chk" violates check constraint "chk_t_check""#, detail: "Failing row contains (B).", schema: "public", table: "chk", constraint: "chk_t_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO gen (t) VALUES ('a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO gen (t) VALUES ('z');",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"null value in column "v" of relation "gen" violates not-null constraint"#, detail: "Failing row contains (z, zx, virtual).", schema: "public", table: "gen", column: "v", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM gen;",
                    expected: Expected::Rows {
                        columns: &[Column("t", TEXT), Column("u", TEXT), Column("v", TEXT)],
                        rows: &[
                            &[T("a"), T("ax"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO trg VALUES ('a'), ('c');",
                    expected: Expected::Tag("INSERT 0 2"),
                    notices: &[Diagnostic { code: "00000", message: "fired for c", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER DOMAIN dom ADD CONSTRAINT dom_check CHECK (VALUE > 'b' COLLATE "C");"#,
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"column "d" of table "domt" contains values that violate the new constraint"#, schema: "public", table: "domt", column: "d", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM domt;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER DOMAIN dom ADD CONSTRAINT dom_check CHECK (VALUE > 'b' COLLATE "C");"#,
                    expected: Expected::Tag("ALTER DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO parted_a VALUES ('b');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "parted_a" violates partition constraint"#, detail: "Failing row contains (b).", schema: "public", table: "parted_a", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_on_commit_delete_rows_keeps_statistics() {
    run_scripts(&[
        ScriptTest {
            name: "ON COMMIT DELETE ROWS keeps a table's statistics and rebuilds its indexes' as Postgres' heap_truncate does",
            set_up_script: &[
                "CREATE TEMP TABLE past_inh_db_other ();",
                "CREATE TEMP TABLE past_inh_db_parent () ON COMMIT DELETE ROWS;",
                "CREATE TEMP TABLE past_inh_db_child () INHERITS (past_inh_db_parent);",
                "CREATE INDEX ON past_inh_db_parent ((1));",
                "CREATE TEMP TABLE oc_analyzed (a int) ON COMMIT DELETE ROWS;",
                "CREATE INDEX ON oc_analyzed (a);",
                "CREATE INDEX ON oc_analyzed USING gin ((ARRAY[a]));",
                "CREATE TEMP TABLE oc_plain () ON COMMIT DELETE ROWS;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE past_inh_db_parent;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reltuples, relhassubclass FROM pg_class WHERE oid = 'past_inh_db_parent'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4), Column("relhassubclass", BOOL)],
                        rows: &[
                            &[T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE past_inh_db_child;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reltuples, relhassubclass FROM pg_class WHERE oid = 'past_inh_db_parent'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4), Column("relhassubclass", BOOL)],
                        rows: &[
                            &[T("0"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc_analyzed SELECT generate_series(1, 100);",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE oc_analyzed;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relpages, reltuples FROM pg_class WHERE relname LIKE 'oc_analyzed%' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("oc_analyzed"), T("0"), T("0")],
                            &[T("oc_analyzed_a_idx"), T("1"), T("0")],
                            &[T("oc_analyzed_array_idx"), T("2"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE oc_unanalyzed (a int) ON COMMIT DELETE ROWS;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON oc_unanalyzed (a);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc_unanalyzed SELECT generate_series(1, 100);",
                    expected: Expected::Tag("INSERT 0 100"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relpages, reltuples FROM pg_class WHERE relname LIKE 'oc_unanalyzed%' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relpages", INT4), Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("oc_unanalyzed"), T("0"), T("-1")],
                            &[T("oc_unanalyzed_a_idx"), T("1"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE oc_plain;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reltuples FROM pg_class WHERE oid = 'oc_plain'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4)],
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
fn test_check_constraints_stop_at_the_first_a_row_fails() {
    run_scripts(&[
        ScriptTest {
            name: "a row's CHECK constraints are evaluated in name order and stop at the first it fails, so a later constraint that cannot evaluate the row never runs",
            set_up_script: &[
                "CREATE TABLE cfirst (js TEXT CONSTRAINT cfirst_a CHECK (js IS JSON) CONSTRAINT cfirst_b CHECK (js::jsonb IS NOT NULL));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO cfirst VALUES ('');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "cfirst" violates check constraint "cfirst_a""#, detail: "Failing row contains ().", schema: "public", table: "cfirst", constraint: "cfirst_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO cfirst VALUES ('{}');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cfirst SET js = 'x';",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "cfirst" violates check constraint "cfirst_a""#, detail: "Failing row contains (x).", schema: "public", table: "cfirst", constraint: "cfirst_a", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
