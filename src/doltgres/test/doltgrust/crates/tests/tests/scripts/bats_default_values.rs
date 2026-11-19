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

//! Dolt's integration-tests/bats/default-values.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_default_values() {
    run_scripts(&[
        ScriptTest {
            name: "default-values: Standard default literal",
            intent: "A literal default fills a column an insert leaves out.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint DEFAULT 2);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Default expression converting to proper column type",
            intent: "An expression default converts to the column's type.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 VARCHAR(20) DEFAULT (GREATEST(2, 1)));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres refuses a default that reads other columns, so this default is a column-free expression that keeps the test's point")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (1), (2), (3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Default literal of different type but implicitly converts",
            intent: "A string literal default converts to the column's type.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint DEFAULT '4');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8)],
                        rows: &[
                            &[T("1"), T("4")],
                            &[T("2"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: TEXT expression",
            intent: "A text column takes an expression default.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 text DEFAULT (77));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", TEXT)],
                        rows: &[
                            &[T("1"), T("77")],
                            &[T("2"), T("77")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: REPLACE INTO with default expression",
            intent: "REPLACE INTO fills a column it leaves out with its expression default.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 smallint DEFAULT (GREATEST(2, 1)));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres refuses a default that reads other columns, so this default is a column-free expression that keeps the test's point")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (2), (3) ON CONFLICT (\"pk\") DO UPDATE SET \"v1\" = EXCLUDED.\"v1\";",
                    expected: Expected::Tag("INSERT 0 2"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's REPLACE INTO is an INSERT ... ON CONFLICT DO UPDATE, whose EXCLUDED row holds the defaults of the columns it leaves out")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT2)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Add column last default literal",
            intent: "A column added with a default fills existing rows with it.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint DEFAULT '4');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD COLUMN v2 bigint DEFAULT 5;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("4"), T("5")],
                            &[T("2"), T("4"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Add column implicit last default expression",
            intent: "A column added with an expression default fills existing rows with it.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint DEFAULT (1 + 1));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres refuses a default that reads other columns, so this default is a column-free expression that keeps the test's point")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD COLUMN v2 bigint DEFAULT (2 + 2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("Postgres refuses a default that reads other columns, so this default is a column-free expression that keeps the test's point")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8), Column("v2", INT8)],
                        rows: &[
                            &[T("1"), T("2"), T("4")],
                            &[T("2"), T("2"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Invalid literal for column type 2",
            intent: "A default that can't convert to the column's type is refused.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint DEFAULT 'hi');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type bigint: \"hi\"", position: 60, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Null literal is invalid for NOT NULL",
            intent: "A NULL default on a NOT NULL column can't be used.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint NOT NULL DEFAULT NULL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres accepts a NOT NULL column's NULL default and fails only when an insert uses it (23502), where MySQL refuses the default")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"v1\" of relation \"test\" violates not-null constraint", detail: "Failing row contains (1, null).", schema: "public", table: "test", column: "v1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Back reference to expression",
            intent: "A default referencing a later column's expression default is refused.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint DEFAULT (v2), v2 bigint DEFAULT (9));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot use column reference in DEFAULT expression", position: 61, ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses every column reference in a default, so this default is refused as Dolt refuses it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: TEXT literals",
            intent: "Text columns take literal defaults.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test1(pk bigint PRIMARY KEY, v1 TEXT DEFAULT 'hi');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2(pk bigint PRIMARY KEY, v1 text DEFAULT 'hi');",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Other types using NOW/CURRENT_TIMESTAMP literal",
            intent: "A current-time default on a column of another type is refused.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint DEFAULT NOW());",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "column \"v1\" is of type bigint but default expression is of type timestamp with time zone", hint: "You will need to rewrite or cast the expression.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 VARCHAR(20) DEFAULT CURRENT_TIMESTAMP);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres assigns a timestamp to a varchar or date column by its assignment cast, where MySQL refuses the default")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 BIT(5) DEFAULT NOW());",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"test\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 DATE DEFAULT CURRENT_TIMESTAMP);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"test\" already exists", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Custom functions are invalid",
            intent: "A default calling an unknown function is refused.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint DEFAULT (CUSTOMFUNC(1)));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function customfunc(integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 61, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Default expression references own column",
            intent: "A default referencing its own column is refused.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint DEFAULT (v1));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot use column reference in DEFAULT expression", position: 61, ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses every column reference in a default, so this default is refused as Dolt refuses it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Expression contains null on NOT NULL, fails on insertion",
            intent: "A NULL default on a NOT NULL column fails when an insert uses it.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint NOT NULL DEFAULT (NULL));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk) VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"v1\" of relation \"test\" violates not-null constraint", detail: "Failing row contains (1, null).", schema: "public", table: "test", column: "v1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Import with default values",
            intent: "Rows that leave a column out take its default.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint DEFAULT 2 NOT NULL, v2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (pk, v2) VALUES (1, 3), (2, 4);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                            &[T("2"), T("2"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT8), Column("v2", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                            &[T("2"), T("2"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Defining literal default value to NULL or EMPTY value in ALTER TABLE",
            intent: "Changing columns sets their nullability and defaults, which information_schema.columns shows.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, c1 bigint, c2 bigint, c3 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'test';",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("is_nullable", VARCHAR), Column("column_default", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("NO"), Null],
                            &[T("c1"), T("YES"), Null],
                            &[T("c2"), T("YES"), Null],
                            &[T("c3"), T("YES"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN c1 DROP DEFAULT, ALTER COLUMN c1 TYPE int USING c1::int, ALTER COLUMN c1 DROP NOT NULL, ALTER COLUMN c1 SET DEFAULT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN c2 DROP DEFAULT, ALTER COLUMN c2 TYPE varchar(4) USING c2::varchar(4), ALTER COLUMN c2 SET NOT NULL, ALTER COLUMN c2 SET DEFAULT '';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN c3 DROP DEFAULT, ALTER COLUMN c3 TYPE varchar(4) USING c3::varchar(4), ALTER COLUMN c3 SET NOT NULL, ALTER COLUMN c3 SET DEFAULT 'ln';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'test';",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("is_nullable", VARCHAR), Column("column_default", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("NO"), Null],
                            &[T("c1"), T("YES"), Null],
                            &[T("c2"), T("NO"), T("''::character varying")],
                            &[T("c3"), T("NO"), T("'ln'::character varying")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' form of each default")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN c1 DROP DEFAULT, ALTER COLUMN c1 TYPE FLOAT USING c1::FLOAT, ALTER COLUMN c1 SET NOT NULL, ALTER COLUMN c1 SET DEFAULT '4.44';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN c2 DROP DEFAULT, ALTER COLUMN c2 TYPE double precision USING c2::double precision, ALTER COLUMN c2 SET NOT NULL, ALTER COLUMN c2 SET DEFAULT '3.333';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN c3 DROP DEFAULT, ALTER COLUMN c3 TYPE BOOLEAN USING c3::BOOLEAN, ALTER COLUMN c3 DROP NOT NULL, ALTER COLUMN c3 SET DEFAULT FALSE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'test';",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("is_nullable", VARCHAR), Column("column_default", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("NO"), Null],
                            &[T("c1"), T("NO"), T("'4.44'::double precision")],
                            &[T("c2"), T("NO"), T("'3.333'::double precision")],
                            &[T("c3"), T("YES"), T("false")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' form of each default")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN c1 DROP DEFAULT, ALTER COLUMN c1 TYPE timestamp USING c1::timestamp, ALTER COLUMN c1 SET NOT NULL, ALTER COLUMN c1 SET DEFAULT '2020-04-01 16:16:16';",
                    expected: Expected::Error(Diagnostic { code: "42846", message: "cannot cast type double precision to timestamp without time zone", position: 87, ..E }),
                    dolt: Some(Dolt::MySqlOnly("Postgres has no cast from double precision to timestamp, which MySQL's CHANGE converts")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN c2 DROP DEFAULT, ALTER COLUMN c2 TYPE TIMESTAMP USING c2::TIMESTAMP, ALTER COLUMN c2 DROP NOT NULL, ALTER COLUMN c2 SET DEFAULT '2008-04-22 16:16:16';",
                    expected: Expected::Error(Diagnostic { code: "42846", message: "cannot cast type double precision to timestamp without time zone", position: 87, ..E }),
                    dolt: Some(Dolt::MySqlOnly("Postgres has no cast from double precision to timestamp, which MySQL's CHANGE converts")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'test';",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("is_nullable", VARCHAR), Column("column_default", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("NO"), Null],
                            &[T("c1"), T("NO"), T("'4.44'::double precision")],
                            &[T("c2"), T("NO"), T("'3.333'::double precision")],
                            &[T("c3"), T("YES"), T("false")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' form of each default, and c1 and c2 stay double precision since Postgres refused their type changes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Function default values are correctly represented in the information_schema.columns table",
            intent: "A function default shows in information_schema.columns.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk bigint PRIMARY KEY, v1 smallint DEFAULT (GREATEST(2, 1)));",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::Differs("Postgres refuses a default that reads other columns, so this default is a column-free expression that keeps the test's point")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'test';",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("is_nullable", VARCHAR), Column("column_default", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("NO"), Null],
                            &[T("v1"), T("YES"), T("GREATEST(2, 1)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Additional test with function defaults",
            intent: "Function defaults set while changing columns show in information_schema.columns.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test_table (pk int PRIMARY KEY, fname varchar(20), lname varchar(20), height int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test_table RENAME COLUMN fname TO col2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test_table ALTER COLUMN col2 DROP DEFAULT, ALTER COLUMN col2 TYPE float USING col2::float, ALTER COLUMN col2 SET NOT NULL, ALTER COLUMN col2 SET DEFAULT (length('hello'));",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test_table RENAME COLUMN lname TO col3;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test_table ALTER COLUMN col3 DROP DEFAULT, ALTER COLUMN col3 TYPE double precision USING col3::double precision, ALTER COLUMN col3 SET NOT NULL, ALTER COLUMN col3 SET DEFAULT (ROUND(-1.58));",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test_table RENAME COLUMN height TO col4;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test_table ALTER COLUMN col4 DROP DEFAULT, ALTER COLUMN col4 TYPE float USING col4::float, ALTER COLUMN col4 DROP NOT NULL, ALTER COLUMN col4 SET DEFAULT (random());",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, column_default FROM information_schema.columns WHERE table_name = 'test_table';",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("column_default", VARCHAR)],
                        rows: &[
                            &[T("pk"), Null],
                            &[T("col2"), T("length('hello'::text)")],
                            &[T("col3"), T("round('-1.58'::numeric)")],
                            &[T("col4"), T("random()")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' form of each default, and MySQL's RAND() is random()")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "default-values: Outputting the string version of a more complex default value works",
            intent: "Expression defaults show in information_schema.columns as text.",
            set_up_script: &[
                "CREATE TABLE parent (id int PRIMARY KEY, v1 int, v2 int); CREATE INDEX v1 ON parent (v1); CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id int PRIMARY KEY, v1 int, v2 int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test_table (pk int PRIMARY KEY, fname varchar(20), lname varchar(20));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test_table RENAME COLUMN fname TO col2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test_table ALTER COLUMN col2 DROP DEFAULT, ALTER COLUMN col2 TYPE int USING col2::int, ALTER COLUMN col2 DROP NOT NULL, ALTER COLUMN col2 SET DEFAULT (random()+random());",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test_table RENAME COLUMN lname TO col3;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test_table ALTER COLUMN col3 DROP DEFAULT, ALTER COLUMN col3 TYPE BOOLEAN USING col3::BOOLEAN, ALTER COLUMN col3 DROP NOT NULL, ALTER COLUMN col3 SET DEFAULT (CASE pk WHEN 1 THEN FALSE ELSE TRUE END);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot use column reference in DEFAULT expression", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL lets a default reference other columns, which Postgres refuses")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, column_default FROM information_schema.columns WHERE table_name='test_table';",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("column_default", VARCHAR)],
                        rows: &[
                            &[T("pk"), Null],
                            &[T("col2"), T("(random() + random())")],
                            &[T("col3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::Differs("information_schema.columns shows Postgres' form of each default, and MySQL's RAND() is random(), and col3 has no default since Postgres refused it")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
