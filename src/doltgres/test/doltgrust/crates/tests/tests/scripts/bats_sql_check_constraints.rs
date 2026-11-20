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

//! Dolt's integration-tests/bats/sql-check-constraints.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_check_constraints() {
    run_scripts(&[
        ScriptTest {
            name: "sql-check-constraints: basic tests for check constraints",
            intent: "Check constraints declared on columns or added later refuse violating rows until they are dropped.",
            set_up_script: &[
                "CREATE TABLE t1 (a integer PRIMARY KEY CHECK (a > 3), b integer CHECK (b > a));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (5, 6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (3, 4);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t1\" violates check constraint \"t1_a_check\"", detail: "Failing row contains (3, 4).", schema: "public", table: "t1", constraint: "t1_a_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (4, 2);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t1\" violates check constraint \"t1_check\"", detail: "Failing row contains (4, 2).", schema: "public", table: "t1", constraint: "t1_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (a integer PRIMARY KEY, b integer);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 ADD CONSTRAINT chk1 CHECK (a > 3);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 ADD CONSTRAINT chk2 CHECK (b > a);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (5, 6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (6, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (3, 4);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t2\" violates check constraint \"chk1\"", detail: "Failing row contains (3, 4).", schema: "public", table: "t2", constraint: "chk1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (4, 2);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t2\" violates check constraint \"chk2\"", detail: "Failing row contains (4, 2).", schema: "public", table: "t2", constraint: "chk2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 DROP CONSTRAINT chk1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (3, 4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (4, 2);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t2\" violates check constraint \"chk2\"", detail: "Failing row contains (4, 2).", schema: "public", table: "t2", constraint: "chk2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 DROP CONSTRAINT chk2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (4, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (4, 2);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t1\" violates check constraint \"t1_check\"", detail: "Failing row contains (4, 2).", schema: "public", table: "t1", constraint: "t1_check", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-check-constraints: check constraints survive adding a new column",
            intent: "A table's check constraint survives adding a column, and information_schema lists it.",
            set_up_script: &[
                "CREATE TABLE foo (pk int, c1 int, CHECK (c1 > 3), PRIMARY KEY (pk));",
                "ALTER TABLE foo ADD COLUMN j int;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'foo'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("CHECK ((c1 > 3))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints the table's statement, whose CHECK pg_get_constraintdef prints")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT constraint_catalog, constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_name LIKE 'foo%';",
                    expected: Expected::Rows {
                        columns: &[Column("constraint_catalog", NAME), Column("constraint_name", NAME), Column("check_clause", VARCHAR)],
                        rows: &[
                            &[T("postgres"), T("foo_c1_check"), T("(c1 > 3)")],
                            &[T("postgres"), T("foo_pk_not_null"), T("pk IS NOT NULL")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Postgres names an unnamed check constraint <table>_<column>_check, where Dolt makes up a random name, and lists the primary key's NOT NULL constraint beside it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-check-constraints: check constraints survive renaming a column",
            intent: "A table's check constraint survives renaming a column.",
            set_up_script: &[
                "CREATE TABLE foo (pk int, c1 int, j int, CHECK (c1 > 3), PRIMARY KEY (pk));",
                "ALTER TABLE foo RENAME COLUMN j TO j2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'foo'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("CHECK ((c1 > 3))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints the table's statement, whose CHECK pg_get_constraintdef prints")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-check-constraints: check constraints survive modifying a column",
            intent: "A table's check constraint survives modifying a column.",
            set_up_script: &[
                "CREATE TABLE foo (pk int, c1 int, j int, CHECK (c1 > 3), PRIMARY KEY (pk));",
                "ALTER TABLE foo ALTER COLUMN j TYPE int;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'foo'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("CHECK ((c1 > 3))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints the table's statement, whose CHECK pg_get_constraintdef prints")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-check-constraints: check constraints survive dropping a column",
            intent: "A table's check constraint survives dropping a column.",
            set_up_script: &[
                "CREATE TABLE foo (pk int, c1 int, j int, CHECK (c1 > 3), PRIMARY KEY (pk));",
                "ALTER TABLE foo DROP COLUMN j;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'foo'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("CHECK ((c1 > 3))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints the table's statement, whose CHECK pg_get_constraintdef prints")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-check-constraints: check constraints survive adding a primary key",
            intent: "A table's check constraint survives adding a primary key.",
            set_up_script: &[
                "CREATE TABLE foo (pk int, c1 int, CHECK (c1 > 3));",
                "ALTER TABLE foo ADD PRIMARY KEY(pk);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'foo'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("CHECK ((c1 > 3))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints the table's statement, whose CHECK pg_get_constraintdef prints")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-check-constraints: check constraints survive dropping a primary key",
            intent: "A table's check constraint survives dropping the primary key.",
            set_up_script: &[
                "CREATE TABLE foo (pk int, c1 int, CHECK (c1 > 3), PRIMARY KEY (pk));",
                "ALTER TABLE foo DROP CONSTRAINT foo_pkey;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'foo'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("CHECK ((c1 > 3))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints the table's statement, whose CHECK pg_get_constraintdef prints")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-check-constraints: check constraints survive renaming a table",
            intent: "A table's check constraint survives renaming the table.",
            set_up_script: &[
                "CREATE TABLE foo (pk int, c1 int, CHECK (c1 > 3), PRIMARY KEY (pk));",
                "ALTER TABLE foo RENAME TO foo2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'foo2'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("CHECK ((c1 > 3))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show prints the table's statement, whose CHECK pg_get_constraintdef prints")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
