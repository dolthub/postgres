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
fn test_alter_table() {
    run_scripts(&[
        ScriptTest {
            name: "Add Foreign Key Constraint",
            set_up_script: &[
                "create table child (pk int primary key, c1 int);",
                "insert into child values (1,1), (2,2), (3,3);",
                "create index idx_child_c1 on child (pk, c1);",
                "create table parent (pk int primary key, c1 int, c2 int);",
                "insert into parent values (1, 1, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE parent ADD FOREIGN KEY (c1) REFERENCES child (pk) ON DELETE CASCADE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO parent VALUES (10, 10, 10);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "parent" violates foreign key constraint "parent_c1_fkey""#, detail: r#"Key (c1)=(10) is not present in table "child"."#, schema: "public", table: "parent", constraint: "parent_c1_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent ADD FOREIGN KEY (c2) REFERENCES child (pk);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "parent" violates foreign key constraint "parent_c2_fkey""#, detail: r#"Key (c2)=(10) is not present in table "child"."#, schema: "public", table: "parent", constraint: "parent_c2_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent ADD FOREIGN KEY (c1, c2) REFERENCES child (pk, c1);",
                    expected: Expected::Error(Diagnostic { code: "42830", message: r#"there is no unique constraint matching given keys for referenced table "child""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent ADD FOREIGN KEY (c1, c2) REFERENCES child (pk, c1) MATCH PARTIAL;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "MATCH PARTIAL not yet implemented", position: 71, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Add Unique Constraint",
            set_up_script: &[
                "create table t1 (pk int primary key, c1 int);",
                "insert into t1 values (1,1);",
                "create table t2 (pk int primary key, c1 int);",
                "insert into t2 values (1,1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX ON t1(c1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (2, 1);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t1_c1_idx""#, detail: "Key (c1)=(1) already exists.", schema: "public", table: "t1", constraint: "t1_c1_idx", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 ADD CONSTRAINT uniq1 UNIQUE (c1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (2, 1);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "uniq1""#, detail: "Key (c1)=(1) already exists.", schema: "public", table: "t2", constraint: "uniq1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Add Check Constraint",
            set_up_script: &[
                "create table t1 (pk int primary key, c1 int);",
                "insert into t1 values (1,1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ADD CONSTRAINT constraint1 CHECK (c1 > 100);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"check constraint "constraint1" of relation "t1" is violated by some row"#, schema: "public", table: "t1", constraint: "constraint1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ADD CONSTRAINT constraint1 CHECK (c1 < 100);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (3, 101);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "t1" violates check constraint "constraint1""#, detail: "Failing row contains (3, 101).", schema: "public", table: "t1", constraint: "constraint1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Add Check Constraint with IN tuple",
            set_up_script: &[
                "create table t1 (pk int primary key, c1 int);",
                "insert into t1 values (1,1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ADD CONSTRAINT constraint1 CHECK (c1 in (100));",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"check constraint "constraint1" of relation "t1" is violated by some row"#, schema: "public", table: "t1", constraint: "constraint1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ADD CONSTRAINT constraint1 CHECK (c1 in (1,2));",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (3, 101);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "t1" violates check constraint "constraint1""#, detail: "Failing row contains (3, 101).", schema: "public", table: "t1", constraint: "constraint1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Add Check Constraint and another constraint in same statement",
            set_up_script: &[
                "create table t1 (pk int, c1 int);",
                "insert into t1 values (1,1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: " ALTER TABLE t1 ADD CONSTRAINT check_a CHECK (c1 IN (1)), ALTER c1 SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "t1" violates check constraint "check_a""#, detail: "Failing row contains (2, 2).", schema: "public", table: "t1", constraint: "check_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (1, NULL);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"null value in column "c1" of relation "t1" violates not-null constraint"#, detail: "Failing row contains (1, null).", schema: "public", table: "t1", column: "c1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Drop Constraint",
            set_up_script: &[
                "create table t1 (pk int primary key, c1 int);",
                "ALTER TABLE t1 ADD CONSTRAINT constraint1 CHECK (c1 > 100);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 DROP CONSTRAINT constraint1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 DROP CONSTRAINT doesnotexist;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"constraint "doesnotexist" of relation "t1" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 DROP CONSTRAINT IF EXISTS doesnotexist;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"constraint "doesnotexist" of relation "t1" does not exist, skipping"#, ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Add Primary Key",
            set_up_script: &[
                "CREATE TABLE test1 (a INT, b INT);",
                "CREATE TABLE test2 (a INT, b INT, c INT);",
                "CREATE TABLE pkTable1 (a INT PRIMARY KEY);",
                "CREATE TABLE duplicateRows (a INT, b INT);",
                "INSERT INTO duplicateRows VALUES (1, 2), (1, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ADD PRIMARY KEY (a);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT into test1 values (1, 2), (1, 3);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "test1_pkey""#, detail: "Key (a)=(1) already exists.", schema: "public", table: "test1", constraint: "test1_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test2 ADD PRIMARY KEY (a, b);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT into test2 values (1, 2, 3), (1, 2, 4);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "test2_pkey""#, detail: "Key (a, b)=(1, 2) already exists.", schema: "public", table: "test2", constraint: "test2_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pkTable1 ADD PRIMARY KEY (a);",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: r#"multiple primary keys for table "pktable1" are not allowed"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE duplicateRows ADD PRIMARY KEY (a);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"could not create unique index "duplicaterows_pkey""#, detail: "Key (a)=(1) is duplicated.", schema: "public", table: "duplicaterows", constraint: "duplicaterows_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE IF EXISTS doesNotExist ADD PRIMARY KEY (a, b);",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"relation "doesnotexist" does not exist, skipping"#, ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Add Primary Key on text column",
            set_up_script: &[
                "CREATE TABLE test1 (a text, b INT);",
                "insert into test1 values ('a', 1), ('b', 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ADD PRIMARY KEY (a);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT into test1 values ('a', 3);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "test1_pkey""#, detail: "Key (a)=(a) already exists.", schema: "public", table: "test1", constraint: "test1_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from test1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", TEXT), Column("b", INT4)],
                        rows: &[
                            &[T("a"), T("1")],
                            &[T("b"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        // Changed from the Go test: Postgres defines gen_random_uuid in pg_catalog, not public.
        ScriptTest {
            name: "Add primary key with generated column",
            set_up_script: &[
                r#"CREATE TABLE t1 (
      id uuid DEFAULT gen_random_uuid() NOT NULL,
      data jsonb,
      has_data boolean GENERATED ALWAYS AS ((data IS NOT NULL)) STORED
  );"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: " ALTER TABLE ONLY t1 ADD CONSTRAINT pk PRIMARY KEY (id);",
                    expected: Expected::Tag("ALTER TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "insert into t1 (id, data) values (default, '{}');",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "Select has_data from t1;",
                    expected: Expected::Rows {
                        columns: &[Column("has_data", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Add Column",
            set_up_script: &[
                "CREATE TABLE test1 (a INT, b INT);",
                "INSERT INTO test1 VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ADD COLUMN c INT NOT NULL DEFAULT 42;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from test1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ADD COLUMN l non_existing_type;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"type "non_existing_type" does not exist"#, position: 32, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ADD COLUMN m xid;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Add column with inline check constraint",
            set_up_script: &[
                "CREATE TABLE test1 (a INT, b INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ADD COLUMN c INT NOT NULL DEFAULT 42 CONSTRAINT chk1 CHECK (c > 0);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test1 VALUES (2, 2, -2);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "test1" violates check constraint "chk1""#, detail: "Failing row contains (2, 2, -2).", schema: "public", table: "test1", constraint: "chk1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Add column with inline check constraint to table with existing data",
            set_up_script: &[
                "CREATE TABLE test1 (a INT, b INT);",
                "INSERT INTO test1 VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ADD COLUMN c INT NOT NULL DEFAULT 42 CONSTRAINT chk1 CHECK (c > 0);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test1 VALUES (2, 2, -2);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "test1" violates check constraint "chk1""#, detail: "Failing row contains (2, 2, -2).", schema: "public", table: "test1", constraint: "chk1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ADD COLUMN c2 INT CONSTRAINT chk2 CHECK (c2 IS NOT NULL);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"check constraint "chk2" of relation "test1" is violated by some row"#, schema: "public", table: "test1", constraint: "chk2", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Drop Column",
            set_up_script: &[
                "CREATE TABLE test1 (a INT, b INT, c INT, d INT);",
                "INSERT INTO test1 VALUES (1, 2, 3, 4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 DROP COLUMN c;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from test1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 DROP COLUMN d;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from test1;",
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
                    query: "ALTER TABLE test1 DROP COLUMN IF EXISTS zzz;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"column "zzz" of relation "test1" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE IF EXISTS doesNotExist DROP COLUMN z;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"relation "doesnotexist" does not exist, skipping"#, ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Rename Column",
            set_up_script: &[
                "CREATE TABLE test1 (a INT, b INT, c INT, d INT);",
                "INSERT INTO test1 VALUES (1, 2, 3, 4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 RENAME COLUMN c to jjj;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from test1 where jjj=3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("jjj", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Set Column Default",
            set_up_script: &[
                "CREATE TABLE test1 (a INT, b INT DEFAULT 42, c INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ALTER COLUMN c SET DEFAULT 43;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test1 (a) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("42"), T("43")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ALTER COLUMN b DROP DEFAULT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test1 (a) VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test1 where a = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("2"), Null, T("43")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ALTER COLUMN c SET DEFAULT length('hello world');",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test1 (a) VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test1 where a = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("3"), Null, T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Set Column Nullability",
            set_up_script: &[
                "CREATE TABLE test1 (a INT, b INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ALTER COLUMN b SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test1 VALUES (1, NULL);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"null value in column "b" of relation "test1" violates not-null constraint"#, detail: "Failing row contains (1, null).", schema: "public", table: "test1", column: "b", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ALTER COLUMN b DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test1 VALUES (2, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test1 where a = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ALTER COLUMN b SET NOT NULL;",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"column "b" of relation "test1" contains null values"#, schema: "public", table: "test1", column: "b", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Alter Column Type",
            set_up_script: &[
                "CREATE TABLE test1 (a INT, b smallint);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test1 VALUES (1, 32769);",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "smallint out of range", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ALTER COLUMN b TYPE INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test1 VALUES (1, 32769);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("32769")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test1 ALTER COLUMN b TYPE smallint;",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "smallint out of range", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Alter Column Type with USING clause",
            set_up_script: &[
                "CREATE TABLE t1 (id INT PRIMARY KEY, c TEXT);",
                "INSERT INTO t1 VALUES (1, '100'), (2, '-42'), (3, NULL);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN c TYPE integer USING c::integer;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c, pg_typeof(c) FROM t1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4), Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("1"), T("100"), T("integer")],
                            &[T("2"), T("-42"), T("integer")],
                            &[T("3"), Null, T("integer")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c FROM t1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
                            &[T("2"), T("-42")],
                            &[T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (4, 'abc');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "abc""#, position: 27, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (4, 999);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN c TYPE text USING 'val: ' || (c * 2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c FROM t1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("val: 200")],
                            &[T("2"), T("val: -84")],
                            &[T("3"), Null],
                            &[T("4"), T("val: 1998")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Alter Column Type with USING expression form",
            set_up_script: &[
                "CREATE TABLE t1 (id INT PRIMARY KEY, c TEXT NOT NULL);",
                "INSERT INTO t1 VALUES (1, '1'), (2, '25');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN c TYPE integer USING (c || '0')::integer;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c FROM t1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("250")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN c TYPE text USING ('id-' || id || ': ' || c);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c FROM t1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("id-1: 10")],
                            &[T("2"), T("id-2: 250")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Alter Column Type with USING in Django migration style",
            set_up_script: &[
                r#"CREATE TABLE "app_event" ("id" integer PRIMARY KEY, "created" text);"#,
                r#"INSERT INTO "app_event" VALUES (1, '2024-01-15 10:30:00+00'), (2, '2025-06-01 08:00:00+00');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE "app_event" ALTER COLUMN "created" TYPE timestamp with time zone USING "created"::timestamp with time zone;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM app_event WHERE created = '2024-01-15 10:30:00+00'::timestamptz;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM app_event WHERE created > '2024-12-31 00:00:00+00'::timestamptz;",
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
            name: "Alter Column Type with USING error cases",
            set_up_script: &[
                "CREATE TABLE t1 (id INT PRIMARY KEY, c TEXT);",
                "INSERT INTO t1 VALUES (1, '100'), (2, 'abc');",
                "CREATE TABLE t2 (id INT PRIMARY KEY, c TEXT NOT NULL);",
                "INSERT INTO t2 VALUES (1, '');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN c TYPE integer USING c::integer;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "abc""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c FROM t1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("100")],
                            &[T("2"), T("abc")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 ALTER COLUMN c TYPE integer USING NULLIF(c, '')::integer;",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"column "c" of relation "t2" contains null values"#, schema: "public", table: "t2", column: "c", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN c TYPE integer USING c::integer, ALTER COLUMN id TYPE bigint;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "abc""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN c TYPE integer USING d::integer;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "d" does not exist"#, position: 50, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN c TYPE integer USING other.c::integer;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"missing FROM-clause entry for table "other""#, position: 50, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN c TYPE varchar(20) USING t1.c || '!';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c FROM t1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("1"), T("100!")],
                            &[T("2"), T("abc!")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE doesnotexist ALTER COLUMN c TYPE integer USING c::integer;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "doesnotexist" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE IF EXISTS doesnotexist ALTER COLUMN c TYPE integer USING c::integer;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"relation "doesnotexist" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fkparent (id TEXT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE fkchild (id INT PRIMARY KEY, p TEXT REFERENCES fkparent(id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fkchild ALTER COLUMN p TYPE integer USING p::integer;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"foreign key constraint "fkchild_p_fkey" cannot be implemented"#, detail: r#"Key columns "p" of the referencing table and "id" of the referenced table are of incompatible types: integer and text."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE fkparent ALTER COLUMN id TYPE integer USING id::integer;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: r#"foreign key constraint "fkchild_p_fkey" cannot be implemented"#, detail: r#"Key columns "p" of the referencing table and "id" of the referenced table are of incompatible types: text and integer."#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Alter Column Type with USING failure leaves the table intact",
            set_up_script: &[
                "CREATE TABLE t1 (id INT PRIMARY KEY, value TEXT);",
                "CREATE INDEX t1_value_idx ON t1 (value);",
                "INSERT INTO t1 VALUES (1, '1'), (2, '2'), (3, '3'), (4, '4'), (5, '5'), (6, '6'), (7, '7'), (8, '8'), (9, '9'), (10, '10');",
                "INSERT INTO t1 SELECT id + 10, (id + 10)::text FROM t1;",
                "INSERT INTO t1 SELECT id + 20, (id + 20)::text FROM t1 WHERE id <= 20;",
                "INSERT INTO t1 SELECT id + 40, (id + 40)::text FROM t1 WHERE id <= 40;",
                "INSERT INTO t1 SELECT id + 80, (id + 80)::text FROM t1 WHERE id <= 20;",
                "INSERT INTO t1 VALUES (101, 'not-a-number');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN value TYPE integer USING value::integer;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "not-a-number""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_typeof(value) FROM t1 WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("text")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("101")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE value = '42';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE value = 'not-a-number';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("101")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (102, 'still-text');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("102")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("102")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "postgres",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_typeof(value) FROM t1 WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("text")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "postgres",
                    password: "password",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN value TYPE integer USING value::integer;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "not-a-number""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("102")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t1 WHERE value = '42';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_typeof(value) FROM t1 WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("text")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Alter Column Type with USING on a schema-qualified table",
            set_up_script: &[
                "CREATE SCHEMA s1;",
                "CREATE TABLE s1.t (id INT PRIMARY KEY, c TEXT);",
                "INSERT INTO s1.t VALUES (1, '7'), (2, '8');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE s1.t ALTER COLUMN c TYPE integer USING c::integer;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c FROM s1.t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("7")],
                            &[T("2"), T("8")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Alter Column Type with USING on keys and indexes",
            set_up_script: &[
                "CREATE TABLE t1 (id TEXT PRIMARY KEY, c INT);",
                "INSERT INTO t1 VALUES ('3', 30), ('1', 10), ('2', 20);",
                "CREATE TABLE t2 (id INT PRIMARY KEY, c TEXT);",
                "CREATE INDEX t2_c_idx ON t2 (c);",
                "INSERT INTO t2 VALUES (1, '100'), (2, '200');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN id TYPE integer USING id::integer;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c FROM t1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (1, 11);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t1_pkey""#, detail: "Key (id)=(1) already exists.", schema: "public", table: "t1", constraint: "t1_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 ALTER COLUMN c TYPE integer USING c::integer;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t2 WHERE c = 200;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
            name: "ALTER COLUMN resolves column default expressions",
            set_up_script: &[
                "CREATE TABLE t1 (id VARCHAR PRIMARY KEY, c1 TIMESTAMP DEFAULT CURRENT_TIMESTAMP);",
                "CREATE TABLE t2 (id VARCHAR PRIMARY KEY, c1 VARCHAR(100) DEFAULT concat('f', 'oo'));",
                "CREATE TABLE t3 (id VARCHAR PRIMARY KEY, c1 VARCHAR(20) NOT NULL DEFAULT CONCAT('f', 'oo'));",
                "CREATE TABLE t4 (id VARCHAR PRIMARY KEY, c1 VARCHAR(100) DEFAULT CONCAT('f', 'oo'));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN c1 SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 ALTER COLUMN c1 TYPE VARCHAR(50);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t3 ALTER COLUMN c1 DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t4 RENAME COLUMN c1 TO ccc1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE ADD COLUMN with inline FK constraint",
            set_up_script: &[
                "create table t (v varchar(100));",
                "create table parent (id int primary key);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN c1 int REFERENCES parent(id);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname AS constraint_name, pg_get_constraintdef(oid) AS constraint_definition FROM pg_constraint WHERE conrelid = 't'::regclass AND contype='f';",
                    expected: Expected::Rows {
                        columns: &[Column("constraint_name", NAME), Column("constraint_definition", TEXT)],
                        rows: &[
                            &[T("t_c1_fkey"), T("FOREIGN KEY (c1) REFERENCES parent(id)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES ('abc', 123);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "t" violates foreign key constraint "t_c1_fkey""#, detail: r#"Key (c1)=(123) is not present in table "parent"."#, schema: "public", table: "t", constraint: "t_c1_fkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Rename table",
            set_up_script: &[
                "create schema s1",
                "create schema s2",
                "CREATE TABLE t1 (a INT, b INT);",
                "INSERT INTO t1 VALUES (1, 2);",
                "CREATE TABLE t2 (c INT, d INT);",
                "INSERT INTO t2 VALUES (3, 4);",
                "create table s1.t1 (e INT, f INT);",
                "INSERT INTO s1.t1 VALUES (5, 6);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE doesnotexist RENAME TO t3;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "doesnotexist" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 RENAME TO t3;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3;",
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
                    query: "SELECT * FROM public.t3;",
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
                    query: "SELECT * FROM t1;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "t1" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t3 RENAME TO t2;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "t2" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE s1.t1 RENAME TO t4;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s1.t4;",
                    expected: Expected::Rows {
                        columns: &[Column("e", INT4), Column("f", INT4)],
                        rows: &[
                            &[T("5"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Rename table with a foreign key",
            set_up_script: &[
                "CREATE TABLE bug8_parent (id integer PRIMARY KEY);",
                "CREATE TABLE bug8_child (id integer PRIMARY KEY, parent_id integer);",
                "ALTER TABLE bug8_child ADD CONSTRAINT bug8_fk FOREIGN KEY (parent_id) REFERENCES bug8_parent(id);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE bug8_child RENAME TO bug8_child2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bug8_parent VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bug8_child2 VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bug8_child2 VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "bug8_child2" violates foreign key constraint "bug8_fk""#, detail: r#"Key (parent_id)=(2) is not present in table "bug8_parent"."#, schema: "public", table: "bug8_child2", constraint: "bug8_fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE bug8_parent RENAME TO bug8_parent2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bug8_child2 VALUES (3, 3);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "bug8_child2" violates foreign key constraint "bug8_fk""#, detail: r#"Key (parent_id)=(3) is not present in table "bug8_parent2"."#, schema: "public", table: "bug8_child2", constraint: "bug8_fk", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Rename table must not collide with other relation types",
            set_up_script: &[
                "CREATE TABLE src_tbl (pk int PRIMARY KEY, v1 int);",
                "CREATE TABLE other_tbl (pk int PRIMARY KEY, v1 int);",
                "CREATE SEQUENCE seq1;",
                "CREATE VIEW view1 AS SELECT pk FROM src_tbl;",
                "CREATE INDEX idx1 ON src_tbl (v1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE src_tbl RENAME TO other_tbl;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "other_tbl" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE src_tbl RENAME TO seq1;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "seq1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE src_tbl RENAME TO view1;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "view1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE src_tbl RENAME TO idx1;",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "idx1" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE src_tbl RENAME TO new_name;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM new_name;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter table owner",
            set_up_script: &[
                "CREATE TABLE t1 (a INT, b INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 OWNER TO new_owner;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "new_owner" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter table add primary key with timestamp column default values",
            set_up_script: &[
                r#"CREATE TABLE t1 (
    id int NOT NULL,
    uid uuid NOT NULL,
    created_at timestamp with time zone DEFAULT now() NOT NULL,
    updated_at timestamp with time zone DEFAULT now() NOT NULL
);"#,
                "INSERT INTO t1 (id, uid) VALUES (1, '00000000-0000-0000-0000-000000000001');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ONLY public.t1 ADD CONSTRAINT t1_pkey PRIMARY KEY (id);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select created_at is not null from t1 where id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select updated_at is not null from t1 where id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select created_at = updated_at from t1 where id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter table add primary key with uuid column default values",
            set_up_script: &[
                r#"CREATE TABLE t1 (
    id int NOT NULL,
    uid uuid default gen_random_uuid() NOT NULL
);"#,
                "INSERT INTO t1 (id) VALUES (1);",
                "INSERT INTO t1 (id) VALUES (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ONLY public.t1 ADD CONSTRAINT t1_pkey PRIMARY KEY (id);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select uid is not null from t1 where id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select uid is not null from t1 where id = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select (select uid from t1 where id = 2) = (select uid from t1 where id = 1);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
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
        ScriptTest {
            name: "alter table drop primary key",
            set_up_script: &[
                "CREATE TABLE t1 (id int PRIMARY KEY);",
                "INSERT INTO t1 (id) VALUES (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 DROP CONSTRAINT t1_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (1), (2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE RENAME with table types",
            set_up_script: &[
                "CREATE TABLE t1a (a INT4, b VARCHAR(3));",
                "CREATE TABLE t1b (a VARCHAR(3), b INT4);",
                "CREATE TABLE t2 (id INT4, t1a t1a, t1b t1b);",
                "INSERT INTO t2 VALUES (1, ROW(1, 'abc'), ROW('abc', 1));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(1,abc)"), T("(abc,1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1a RENAME TO t1x;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (2, ROW(2, 'def'), ROW('def', 2));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(1,abc)"), T("(abc,1)")],
                            &[T("2"), T("(2,def)"), T("(def,2)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1x RENAME TO t1y;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (3, ROW(4, 'ghi'), ROW('kjl', 5));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(1,abc)"), T("(abc,1)")],
                            &[T("2"), T("(2,def)"), T("(def,2)")],
                            &[T("3"), T("(4,ghi)"), T("(kjl,5)")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE RENAME COLUMN with table types",
            set_up_script: &[
                "CREATE TABLE t1a (a INT4, b VARCHAR(3));",
                "CREATE TABLE t1b (a VARCHAR(3), b INT4);",
                "CREATE TABLE t2 (id INT4, t1a t1a, t1b t1b);",
                "INSERT INTO t2 VALUES (1, ROW(2, 'abc'), ROW('def', 3));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(2,abc)"), T("(def,3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1a RENAME COLUMN a TO x;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (t1a).a FROM t2;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "a" not found in data type t1a"#, position: 9, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (t1a).x, (t1a).@1 FROM t2;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "@""#, position: 23, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1b RENAME COLUMN b TO bb;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1b RENAME COLUMN a TO aa;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(2,abc)"), T("(def,3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (4, ROW(5, 'ghi'), ROW('jkl', 6));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (t1b).aa, (t1b).@1, (t1b).bb, (t1b).@2 FROM t2;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "@""#, position: 24, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE SET DEFAULT with table types",
            set_up_script: &[
                "CREATE TABLE t1a (a INT4, b VARCHAR(3));",
                "CREATE TABLE t1b (a VARCHAR(3), b INT4);",
                "CREATE TABLE t2 (id INT4, t1a t1a, t1b t1b);",
                "INSERT INTO t2 VALUES (1, ROW(2, 'abc'), ROW('def', 3));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(2,abc)"), T("(def,3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1a ALTER COLUMN a SET DEFAULT 55;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1b ALTER COLUMN b SET DEFAULT 77, ALTER COLUMN a SET DEFAULT 'hi';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (4, ROW(5, 'ghi'), ROW('kjl', 6));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(2,abc)"), T("(def,3)")],
                            &[T("4"), T("(5,ghi)"), T("(kjl,6)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE DROP DEFAULT with table types",
            set_up_script: &[
                "CREATE TABLE t1a (a INT4 DEFAULT 55, b VARCHAR(3) DEFAULT 'hi');",
                "CREATE TABLE t1b (a VARCHAR(5) DEFAULT 'hello', b INT4 DEFAULT 77);",
                "CREATE TABLE t2 (id INT4, t1a t1a, t1b t1b);",
                "INSERT INTO t2 VALUES (1, ROW(2, 'abc'), ROW('def', 3));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(2,abc)"), T("(def,3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1a ALTER COLUMN a DROP DEFAULT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1a ALTER COLUMN b DROP DEFAULT, ALTER COLUMN a DROP DEFAULT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (4, ROW(5, 'ghi'), ROW('kjl', 6));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(2,abc)"), T("(def,3)")],
                            &[T("4"), T("(5,ghi)"), T("(kjl,6)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE SET DATA TYPE with table types",
            set_up_script: &[
                "CREATE TABLE t1a (a INT4, b VARCHAR(3));",
                "CREATE TABLE t1b (a VARCHAR(3), b INT4);",
                "CREATE TABLE t2 (id INT4, t1a t1a, t1b t1b);",
                "INSERT INTO t2 VALUES (1, ROW(2, 'abc'), ROW('def', 3));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(2,abc)"), T("(def,3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1a ALTER COLUMN a SET DATA TYPE INT8;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cannot alter table "t1a" because column "t2.t1a" uses its row type"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1a ALTER COLUMN a SET DATA TYPE INT4;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cannot alter table "t1a" because column "t2.t1a" uses its row type"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 DROP COLUMN t1a;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1a ALTER COLUMN a SET DATA TYPE INT8, ALTER COLUMN b SET DATA TYPE TEXT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(def,3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE SET/DROP NOT NULL with table types",
            set_up_script: &[
                "CREATE TABLE t1a (a INT4, b VARCHAR(3));",
                "CREATE TABLE t1b (a VARCHAR(3), b INT4);",
                "CREATE TABLE t2 (id INT4, t1a t1a, t1b t1b);",
                "INSERT INTO t2 VALUES (1, ROW(2, 'abc'), ROW('def', 3));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(2,abc)"), T("(def,3)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1a ALTER COLUMN a SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1b ALTER COLUMN b SET NOT NULL, ALTER COLUMN a SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1a VALUES (NULL, 'hi');",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"null value in column "a" of relation "t1a" violates not-null constraint"#, detail: "Failing row contains (null, hi).", schema: "public", table: "t1a", column: "a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (4, ROW(NULL, 'ghi'), ROW(NULL, 6));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(2,abc)"), T("(def,3)")],
                            &[T("4"), T("(,ghi)"), T("(,6)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t1b ALTER COLUMN b DROP NOT NULL, ALTER COLUMN a DROP NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("t1a", USER_DEFINED), Column("t1b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("(2,abc)"), T("(def,3)")],
                            &[T("4"), T("(,ghi)"), T("(,6)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE RENAME on view",
            set_up_script: &[
                r#"CREATE TABLE tenk1 (
	unique1		int4,
	unique2		int4,
	two			int4,
	four		int4,
	ten			int4,
	twenty		int4,
	hundred		int4,
	thousand	int4,
	twothousand	int4,
	fivethous	int4,
	tenthous	int4,
	odd			int4,
	even		int4,
	stringu1	name,
	stringu2	name,
	string4		name);"#,
                "CREATE VIEW attmp_view (unique1) AS SELECT unique1 FROM tenk1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp_view RENAME TO attmp_view_new;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE IF EXISTS",
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE IF EXISTS t1a ALTER COLUMN a SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"relation "t1a" does not exist, skipping"#, ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE with schema defined that is not the current schema",
            set_up_script: &[
                "CREATE SCHEMA grassroots;",
                r#"CREATE TYPE grassroots.user_role AS ENUM (
					'ADMIN',
					'USER'
				);"#,
                r#"CREATE TABLE grassroots.users (
					id uuid DEFAULT gen_random_uuid() NOT NULL,
					email text NOT NULL,
					password_hash text NOT NULL,
					first_name text,
					last_name text,
					role grassroots.user_role DEFAULT 'USER'::grassroots.user_role NOT NULL
				);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ONLY grassroots.users ADD CONSTRAINT users_email_key UNIQUE (email);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "setting foreign key to SET NULL on NOT NULL defined column",
            set_up_script: &[
                r#"CREATE TABLE public.products (
		   product_id integer NOT NULL,
		   product_name character varying(100) NOT NULL,
		   category_id integer NOT NULL,
		   price numeric(10,2) NOT NULL,
		   description text
		);"#,
                r#"INSERT INTO public.products VALUES
		                               (13, 'Smartphone', 1, 599.99, 'Latest model with advanced features'),
		                               (14, 'Laptop', 1, 999.99, 'High performance laptop with 16GB RAM'),
		                               (18, 'Novel', 2, 19.99, 'Bestselling fiction novel');"#,
                r#"CREATE TABLE public.categories (
		   category_id integer NOT NULL,
		   category_name character varying(50) NOT NULL
		);"#,
                "INSERT INTO public.categories VALUES (1, 'Electronics'), (2, 'Books'), (3, 'Clothing');",
                r#"ALTER TABLE ONLY public.products
		   ADD CONSTRAINT products_pkey PRIMARY KEY (product_id);"#,
                r#"ALTER TABLE ONLY public.categories
		   ADD CONSTRAINT categories_pkey PRIMARY KEY (category_id);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE ONLY public.products
		   ADD CONSTRAINT fk_category_id FOREIGN KEY (category_id) REFERENCES public.categories(category_id) ON UPDATE SET NULL ON DELETE SET NULL;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM categories WHERE category_id = 1;",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"null value in column "category_id" of relation "products" violates not-null constraint"#, detail: "Failing row contains (13, Smartphone, null, 599.99, Latest model with advanced features).", schema: "public", table: "products", column: "category_id", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "use double quoted column name for SET DEFAULT",
            set_up_script: &[
                r#" CREATE TABLE pages_links (
		"idRefferer" bigint NOT NULL,
		"idDestination" bigint NOT NULL
	);"#,
                r#"CREATE SEQUENCE pages_links_iddestination_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;"#,
                r#"ALTER SEQUENCE pages_links_iddestination_seq OWNED BY pages_links."idDestination";"#,
                r#"CREATE SEQUENCE pages_links_idrefferer_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;"#,
                r#"ALTER SEQUENCE pages_links_idrefferer_seq OWNED BY pages_links."idRefferer";"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE ONLY pages_links ALTER COLUMN "idRefferer" SET DEFAULT nextval('pages_links_idrefferer_seq'::regclass);"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "foreign key names can be the same but should be on different tables",
            set_up_script: &[
                r#" CREATE TABLE public.boards (
    id uuid DEFAULT gen_random_uuid() NOT NULL,
    created_at timestamp without time zone DEFAULT CURRENT_TIMESTAMP,
    updated_at timestamp without time zone DEFAULT CURRENT_TIMESTAMP,
    title character varying(255),
    project_id uuid
);"#,
                r#"CREATE TABLE public.project_assignments (
    id uuid DEFAULT gen_random_uuid() NOT NULL,
    role character varying(20) DEFAULT 'viewer'::character varying,
    created_at timestamp without time zone DEFAULT CURRENT_TIMESTAMP,
    updated_at timestamp without time zone DEFAULT CURRENT_TIMESTAMP,
    user_id uuid NOT NULL,
    project_id uuid NOT NULL
);"#,
                r#"CREATE TABLE public.projects (
    id uuid DEFAULT gen_random_uuid() NOT NULL,
    title character varying(100) NOT NULL,
    description text,
    created_at timestamp without time zone DEFAULT CURRENT_TIMESTAMP,
    updated_at timestamp without time zone DEFAULT CURRENT_TIMESTAMP,
    owner_id uuid
);"#,
                r#"ALTER TABLE ONLY public.boards
    ADD CONSTRAINT boards_pkey PRIMARY KEY (id);"#,
                r#"ALTER TABLE ONLY public.project_assignments
    ADD CONSTRAINT project_assignments_pkey PRIMARY KEY (id);"#,
                r#"ALTER TABLE ONLY public.projects
    ADD CONSTRAINT projects_pkey PRIMARY KEY (id);"#,
                r#"ALTER TABLE ONLY public.project_assignments
    ADD CONSTRAINT fk_project_id FOREIGN KEY (project_id) REFERENCES public.projects(id) ON DELETE CASCADE;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE ONLY public.boards
    ADD CONSTRAINT fk_project_id FOREIGN KEY (project_id) REFERENCES public.projects(id) ON DELETE CASCADE;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE with NOT VALID clauses on foreign key constraint",
            set_up_script: &[
                "CREATE TABLE attmp2 (a int primary key);",
                "CREATE TABLE attmp3 (a int, b int);",
                "INSERT INTO attmp2 values (1),(2),(3),(4);",
                "INSERT INTO attmp3 values (1,10),(1,20),(3, 22),(5,50);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 add constraint attmpconstr foreign key(c) references attmp2(a);",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "c" referenced in foreign key constraint does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 add constraint attmpconstr foreign key(a) references attmp2(b);",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "b" referenced in foreign key constraint does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 add constraint attmpconstr foreign key (a) references attmp2(a);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "attmp3" violates foreign key constraint "attmpconstr""#, detail: r#"Key (a)=(5) is not present in table "attmp2"."#, schema: "public", table: "attmp3", constraint: "attmpconstr", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 add constraint attmpconstr foreign key (a) references attmp2 NOT VALID;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 add constraint attmpconstr foreign key (a) references attmp2 (a) NOT VALID;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"constraint "attmpconstr" for relation "attmp3" already exists"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 validate constraint attmpconstr;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "attmp3" violates foreign key constraint "attmpconstr""#, detail: r#"Key (a)=(5) is not present in table "attmp2"."#, schema: "public", table: "attmp3", constraint: "attmpconstr", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM attmp3 where a=5;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 validate constraint attmpconstr;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 validate constraint attmpconstr;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO attmp3 VALUES (6, 5);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "attmp3" violates foreign key constraint "attmpconstr""#, detail: r#"Key (a)=(6) is not present in table "attmp2"."#, schema: "public", table: "attmp3", constraint: "attmpconstr", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO attmp2 VALUES (6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO attmp3 VALUES (6, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE attmp3 SET a=7 where b=22;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "attmp3" violates foreign key constraint "attmpconstr""#, detail: r#"Key (a)=(7) is not present in table "attmp2"."#, schema: "public", table: "attmp3", constraint: "attmpconstr", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE attmp3 SET a=2 where b=22;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 
    con.conname AS constraint_name,
    cl_child.relname AS child_table,
    (SELECT array_agg(attname) FROM pg_attribute WHERE attrelid = con.conrelid AND attnum = ANY(con.conkey)) AS child_columns,
    cl_parent.relname AS parent_table,
    (SELECT array_agg(attname) FROM pg_attribute WHERE attrelid = con.confrelid AND attnum = ANY(con.confkey)) AS parent_columns
FROM pg_catalog.pg_constraint con
JOIN pg_catalog.pg_class cl_child ON con.conrelid = cl_child.oid
JOIN pg_catalog.pg_class cl_parent ON con.confrelid = cl_parent.oid
WHERE con.contype = 'f';"#,
                    expected: Expected::Rows {
                        columns: &[Column("constraint_name", NAME), Column("child_table", NAME), Column("child_columns", NAME_ARRAY), Column("parent_table", NAME), Column("parent_columns", NAME_ARRAY)],
                        rows: &[
                            &[T("attmpconstr"), T("attmp3"), T("{a}"), T("attmp2"), T("{a}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE with NOT VALID clauses on check constraint",
            set_up_script: &[
                "CREATE TABLE attmp3 (a int, b int);",
                "INSERT INTO attmp3 values (1,10),(1,20),(3,22);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 ADD CONSTRAINT b_greater_than_ten CHECK (b > 10);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"check constraint "b_greater_than_ten" of relation "attmp3" is violated by some row"#, schema: "public", table: "attmp3", constraint: "b_greater_than_ten", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 ADD CONSTRAINT b_greater_than_ten CHECK (b > 10) NOT VALID;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 VALIDATE CONSTRAINT b_greater_than_ten;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"check constraint "b_greater_than_ten" of relation "attmp3" is violated by some row"#, schema: "public", table: "attmp3", constraint: "b_greater_than_ten", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM attmp3 WHERE NOT b > 10;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 VALIDATE CONSTRAINT b_greater_than_ten;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE attmp3 VALIDATE CONSTRAINT b_greater_than_ten;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO attmp3 VALUES (5, 9);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "attmp3" violates check constraint "b_greater_than_ten""#, detail: "Failing row contains (5, 9).", schema: "public", table: "attmp3", constraint: "b_greater_than_ten", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO attmp3 VALUES (6, 11);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE attmp3 SET b=7 where a=3;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "attmp3" violates check constraint "b_greater_than_ten""#, detail: "Failing row contains (3, 7).", schema: "public", table: "attmp3", constraint: "b_greater_than_ten", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE attmp3 SET b=77 where a=3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT                                                              
    ns.nspname AS schema_name,
    cl.relname AS table_name,
    con.conname AS constraint_name,
    pg_get_constraintdef(con.oid) AS constraint_definition
FROM pg_catalog.pg_constraint con
JOIN pg_catalog.pg_class cl ON con.conrelid = cl.oid
JOIN pg_catalog.pg_namespace ns ON cl.relnamespace = ns.oid
WHERE con.contype = 'c'
ORDER BY schema_name, table_name;"#,
                    expected: Expected::Rows {
                        columns: &[Column("schema_name", NAME), Column("table_name", NAME), Column("constraint_name", NAME), Column("constraint_definition", TEXT)],
                        rows: &[
                            &[T("public"), T("attmp3"), T("b_greater_than_ten"), T("CHECK ((b > 10))")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "VALIDATE CONSTRAINT with the table's schema not on the search path",
            set_up_script: &[
                "CREATE TABLE public.vc (a int, b int);",
                "INSERT INTO public.vc VALUES (1, 10);",
                "ALTER TABLE public.vc ADD CONSTRAINT b_gt_ten CHECK (b > 10) NOT VALID;",
                "SELECT pg_catalog.set_config('search_path', '', false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE public.vc VALIDATE CONSTRAINT b_gt_ten;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"check constraint "b_gt_ten" of relation "vc" is violated by some row"#, schema: "public", table: "vc", constraint: "b_gt_ten", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "VALIDATE CONSTRAINT with a same-named table earlier on the search path",
            set_up_script: &[
                "CREATE SCHEMA s2;",
                "CREATE TABLE public.vc (a int, b int);",
                "INSERT INTO public.vc VALUES (11, 5);",
                "ALTER TABLE public.vc ADD CONSTRAINT b_gt_ten CHECK (b > 10) NOT VALID;",
                "CREATE TABLE s2.vc (b int, a int);",
                "SET search_path TO s2, public;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE public.vc VALIDATE CONSTRAINT b_gt_ten;",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"check constraint "b_gt_ten" of relation "vc" is violated by some row"#, schema: "public", table: "vc", constraint: "b_gt_ten", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "duplicate key handling after ADD COLUMN with an expression index",
            set_up_script: &[
                "CREATE TABLE expression_index_alter (id int PRIMARY KEY, name text UNIQUE);",
                "CREATE INDEX expression_index_alter_lower_name ON expression_index_alter ((lower(name)));",
                "ALTER TABLE expression_index_alter ADD COLUMN extra timestamptz;",
                "INSERT INTO expression_index_alter (id, name) VALUES (1, 'v1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO expression_index_alter (id, name) VALUES (1, 'duplicate');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "expression_index_alter_pkey""#, detail: "Key (id)=(1) already exists.", schema: "public", table: "expression_index_alter", constraint: "expression_index_alter_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO expression_index_alter (id, name) VALUES (2, 'v1');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "expression_index_alter_name_key""#, detail: "Key (name)=(v1) already exists.", schema: "public", table: "expression_index_alter", constraint: "expression_index_alter_name_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO expression_index_alter (id, name) VALUES (1, 'v2') ON CONFLICT (id) DO UPDATE SET name = 'v2';",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO expression_index_alter (id, name) VALUES (2, 'v2');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "expression_index_alter_name_key""#, detail: "Key (name)=(v2) already exists.", schema: "public", table: "expression_index_alter", constraint: "expression_index_alter_name_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO expression_index_alter (id, name) VALUES (1, 'ignored') ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO expression_index_alter (id, name) VALUES (1, 'ignored') ON CONFLICT (id) DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, name, extra FROM expression_index_alter;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT), Column("extra", TIMESTAMPTZ)],
                        rows: &[
                            &[T("1"), T("v2"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER TABLE with MATCH FULL on foreign key",
            set_up_script: &[
                "CREATE TABLE parent_table (parent_id INT, sub_id INT, name TEXT, PRIMARY KEY (parent_id, sub_id));",
                "CREATE TABLE child_table (child_id INT PRIMARY KEY, parent_id INT, sub_id INT, description TEXT);",
                "INSERT INTO parent_table (parent_id, sub_id, name) VALUES (1, 10, 'Parent Alpha'), (2, 20, 'Parent Beta'), (2, 40, 'Parent 1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE child_table add constraint constr FOREIGN KEY (parent_id, sub_id) REFERENCES parent_table(parent_id, sub_id) MATCH FULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child_table (child_id, parent_id, sub_id, description) VALUES (101, 1, 10, 'Valid reference');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child_table (child_id, parent_id, sub_id, description) VALUES (102, NULL, NULL, 'Completely unlinked row');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child_table (child_id, parent_id, sub_id, description) VALUES (105, 2, 30, NULL);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "child_table" violates foreign key constraint "constr""#, detail: r#"Key (parent_id, sub_id)=(2, 30) is not present in table "parent_table"."#, schema: "public", table: "child_table", constraint: "constr", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child_table (child_id, parent_id, sub_id, description) VALUES (105, 2, 20, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child_table (child_id, parent_id, sub_id, description) VALUES (103, 1, NULL, 'Partial null mix');",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "child_table" violates foreign key constraint "constr""#, detail: "MATCH FULL does not allow mixing of null and nonnull key values.", schema: "public", table: "child_table", constraint: "constr", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE child_table SET sub_id = NULL where parent_id = 2;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "child_table" violates foreign key constraint "constr""#, detail: "MATCH FULL does not allow mixing of null and nonnull key values.", schema: "public", table: "child_table", constraint: "constr", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE child_table SET sub_id = 40 where parent_id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ADD COLUMN IF NOT EXISTS",
            set_up_script: &[
                "CREATE TABLE t7 (a INT);",
                "INSERT INTO t7 VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t7 ADD COLUMN a INT;",
                    expected: Expected::Error(Diagnostic { code: "42701", message: r#"column "a" of relation "t7" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t7 ADD COLUMN IF NOT EXISTS a INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "42701", message: r#"column "a" of relation "t7" already exists, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t7 ADD COLUMN IF NOT EXISTS b INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t7 ADD COLUMN IF NOT EXISTS a TEXT DEFAULT 'x' UNIQUE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "42701", message: r#"column "a" of relation "t7" already exists, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t7 ADD IF NOT EXISTS c INT, ADD COLUMN IF NOT EXISTS a INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "42701", message: r#"column "a" of relation "t7" already exists, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t7;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 't7' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR)],
                        rows: &[
                            &[T("a"), T("integer")],
                            &[T("b"), T("integer")],
                            &[T("c"), T("integer")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 't7';",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "generated column with COALESCE keeps working after ADD PRIMARY KEY",
            set_up_script: &[
                "CREATE TABLE t (a int NOT NULL, b int GENERATED ALWAYS AS (COALESCE(a + 1, 0)) STORED);",
                "INSERT INTO t (a) VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (a);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (a) VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
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
            name: "generated columns with function calls keep working after other alterations",
            set_up_script: &[
                "CREATE TABLE t1 (a INT PRIMARY KEY, b INT GENERATED ALWAYS AS (a + 1) STORED);",
                "INSERT INTO t1 (a) VALUES (1);",
                "ALTER TABLE t1 ADD COLUMN c INT DEFAULT 0;",
                "CREATE TABLE t2 (a INT PRIMARY KEY, b TEXT GENERATED ALWAYS AS (upper(a::text)) STORED);",
                "INSERT INTO t2 (a) VALUES (1);",
                "ALTER TABLE t2 ADD COLUMN c INT DEFAULT 0;",
                "CREATE TABLE t3 (a INT NOT NULL, b INT GENERATED ALWAYS AS (COALESCE(abs(a), 0)) STORED, c INT);",
                "INSERT INTO t3 (a) VALUES (1);",
                "ALTER TABLE t3 ADD UNIQUE (a);",
                "INSERT INTO t3 (a) VALUES (2);",
                "ALTER TABLE t3 DROP COLUMN c;",
                "INSERT INTO t3 (a) VALUES (3);",
                "UPDATE t3 SET a = 4 WHERE a = 3;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t1 (a) VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("0")],
                            &[T("2"), T("3"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 (a) VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("0")],
                            &[T("2"), T("2"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t3 (a) VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t3 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DEFERRABLE constraints",
            set_up_script: &[
                "CREATE TABLE p (id INTEGER PRIMARY KEY);",
                "CREATE TABLE c (x INTEGER);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD CONSTRAINT c_fk FOREIGN KEY (x) REFERENCES p(id) DEFERRABLE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD CONSTRAINT c_u UNIQUE (x) DEFERRABLE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD CONSTRAINT c_pk PRIMARY KEY (x) INITIALLY DEFERRED;",
                    expected: Expected::Tag("ALTER TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD CONSTRAINT c_chk CHECK (x > 0) DEFERRABLE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "CHECK constraints cannot be marked DEFERRABLE", position: 50, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD COLUMN y INTEGER REFERENCES p(id) DEFERRABLE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD COLUMN z INTEGER UNIQUE DEFERRABLE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD COLUMN w INTEGER CHECK (w > 0) DEFERRABLE;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "misplaced DEFERRABLE clause", position: 50, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD CONSTRAINT c_fk2 FOREIGN KEY (x) REFERENCES p(id) NOT DEFERRABLE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DEFERRABLE constraints are stored",
            set_up_script: &[
                "CREATE TABLE p (id INTEGER PRIMARY KEY);",
                "CREATE TABLE c (x INTEGER);",
                "CREATE TABLE c2 (x INTEGER);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD CONSTRAINT c_fk FOREIGN KEY (x) REFERENCES p(id) DEFERRABLE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD CONSTRAINT c_u UNIQUE (x) DEFERRABLE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD COLUMN y INTEGER REFERENCES p(id) DEFERRABLE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD COLUMN z INTEGER UNIQUE DEFERRABLE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c2 ADD CONSTRAINT c2_pk PRIMARY KEY (x) INITIALLY DEFERRED;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ADD CONSTRAINT c_fk2 FOREIGN KEY (x) REFERENCES p(id) NOT DEFERRABLE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ALTER CONSTRAINT c_fk2 DEFERRABLE INITIALLY DEFERRED;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, condeferrable, condeferred FROM pg_constraint WHERE conname = 'c_fk2';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("condeferrable", BOOL), Column("condeferred", BOOL)],
                        rows: &[
                            &[T("c_fk2"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ALTER CONSTRAINT c_fk2 NOT DEFERRABLE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, condeferrable, condeferred FROM pg_constraint WHERE conname = 'c_fk2';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("condeferrable", BOOL), Column("condeferred", BOOL)],
                        rows: &[
                            &[T("c_fk2"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ALTER CONSTRAINT c_u DEFERRABLE;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"constraint "c_u" of relation "c" is not a foreign key constraint"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, condeferrable, condeferred FROM pg_constraint WHERE connamespace = 'public'::regnamespace AND condeferrable ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("condeferrable", BOOL), Column("condeferred", BOOL)],
                        rows: &[
                            &[T("c2_pk"), T("t"), T("t")],
                            &[T("c_fk"), T("t"), T("f")],
                            &[T("c_u"), T("t"), T("f")],
                            &[T("c_y_fkey"), T("t"), T("f")],
                            &[T("c_z_key"), T("t"), T("f")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "column defaults with user-defined functions keep working after table rewrites",
            set_up_script: &[
                "CREATE FUNCTION f() RETURNS INT LANGUAGE SQL AS $$ SELECT 1 $$;",
                "CREATE TABLE t (id INT, a INT DEFAULT f());",
                "INSERT INTO t (id) VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a TYPE BIGINT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD COLUMN b INT DEFAULT 2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP COLUMN b;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (id);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (id) VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT8)],
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
            name: "NOT NULL constraint names",
            set_up_script: &[
                "CREATE TABLE nn1 (id INT PRIMARY KEY, a INT NOT NULL);",
                "CREATE TABLE nn2 (id INT CONSTRAINT keep NOT NULL, PRIMARY KEY (id));",
                "CREATE TABLE nn3 (LIKE nn1);",
                "ALTER TABLE nn1 RENAME COLUMN a TO b;",
                "ALTER TABLE nn1 RENAME TO nn4;",
                "CREATE TABLE nn1 (x INT);",
                "ALTER TABLE nn1 ADD CONSTRAINT xx NOT NULL x;",
                "ALTER TABLE nn1 ALTER x DROP NOT NULL;",
                "ALTER TABLE nn1 ALTER x SET NOT NULL;",
                "CREATE TABLE nn5 (a INT NOT NULL, b INT, c INT PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT conrelid::regclass, conname, contype FROM pg_constraint WHERE conrelid IN ('nn1'::regclass, 'nn2'::regclass, 'nn3'::regclass, 'nn4'::regclass) ORDER BY conrelid::regclass::text, conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conrelid", REGCLASS), Column("conname", NAME), Column("contype", CHAR)],
                        rows: &[
                            &[T("nn1"), T("nn1_x_not_null"), T("n")],
                            &[T("nn2"), T("keep"), T("n")],
                            &[T("nn2"), T("nn2_pkey"), T("p")],
                            &[T("nn3"), T("nn1_a_not_null"), T("n")],
                            &[T("nn3"), T("nn1_id_not_null"), T("n")],
                            &[T("nn4"), T("nn1_a_not_null"), T("n")],
                            &[T("nn4"), T("nn1_id_not_null"), T("n")],
                            &[T("nn4"), T("nn1_pkey"), T("p")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE nn5 ADD CONSTRAINT other NOT NULL a;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot create not-null constraint "other" on column "a" of table "nn5""#, detail: r#"A not-null constraint named "nn5_a_not_null" already exists for this column."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE nn5 ADD CONSTRAINT bnn NOT NULL b;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE nn5 RENAME CONSTRAINT bnn TO bnn2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname FROM pg_constraint WHERE conrelid = 'nn5'::regclass AND contype = 'n' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME)],
                        rows: &[
                            &[T("bnn2")],
                            &[T("nn5_a_not_null")],
                            &[T("nn5_c_not_null")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE nn5 DROP CONSTRAINT nn5_c_not_null;",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: r#"column "c" is in a primary key"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE nn5 DROP CONSTRAINT bnn2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attnotnull FROM pg_attribute WHERE attrelid = 'nn5'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL)],
                        rows: &[
                            &[T("a"), T("t")],
                            &[T("b"), T("f")],
                            &[T("c"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_table_checks_existing_rows() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER TABLE checks the existing rows against new constraints and new columns",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE p (id int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO p VALUES (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE c (id int PRIMARY KEY, pid int, v int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO c VALUES (1, 1, 5), (2, 3, NULL), (3, NULL, -1);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c ADD CONSTRAINT c_fk FOREIGN KEY (pid) REFERENCES p (id);"#,
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "c" violates foreign key constraint "c_fk""#, detail: r#"Key (pid)=(3) is not present in table "p"."#, schema: "public", table: "c", constraint: "c_fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c ADD CONSTRAINT c_fk FOREIGN KEY (pid) REFERENCES p (id) NOT VALID;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c VALIDATE CONSTRAINT c_fk;"#,
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "c" violates foreign key constraint "c_fk""#, detail: r#"Key (pid)=(3) is not present in table "p"."#, schema: "public", table: "c", constraint: "c_fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM c WHERE id = 2;"#,
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c VALIDATE CONSTRAINT c_fk;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c ADD CONSTRAINT c_v CHECK (v > 0);"#,
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"check constraint "c_v" of relation "c" is violated by some row"#, schema: "public", table: "c", constraint: "c_v", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c ADD CONSTRAINT c_v CHECK (v > 0) NOT VALID;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c VALIDATE CONSTRAINT c_v;"#,
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"check constraint "c_v" of relation "c" is violated by some row"#, schema: "public", table: "c", constraint: "c_v", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"UPDATE c SET v = 7 WHERE id = 3;"#,
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c VALIDATE CONSTRAINT c_v;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c ADD CONSTRAINT c_v2 CHECK (v < 100);"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO c VALUES (4, NULL, NULL);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c ALTER COLUMN v SET NOT NULL;"#,
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"column "v" of relation "c" contains null values"#, schema: "public", table: "c", column: "v", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c ADD CONSTRAINT c_pid_nn NOT NULL pid NOT VALID;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c VALIDATE CONSTRAINT c_pid_nn;"#,
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"column "pid" of relation "c" contains null values"#, schema: "public", table: "c", column: "pid", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM c WHERE pid IS NULL;"#,
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c VALIDATE CONSTRAINT c_pid_nn;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c ALTER COLUMN v SET NOT NULL;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c ADD COLUMN w int NOT NULL;"#,
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"column "w" of relation "c" contains null values"#, schema: "public", table: "c", column: "w", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c ADD COLUMN w int DEFAULT 1;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c ADD COLUMN w2 int DEFAULT NULL;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE e (id int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE e ADD COLUMN w int NOT NULL DEFAULT 5;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE e ADD COLUMN x int DEFAULT 1/0;"#,
                    expected: Expected::Error(Diagnostic { code: "22012", message: r#"division by zero"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DOMAIN pos AS int CHECK (VALUE > 0) NOT NULL;"#,
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE c ADD COLUMN z pos;"#,
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"domain pos does not allow null values"#, schema: "public", data_type: "pos", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE e ADD COLUMN z pos;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE f (id int PRIMARY KEY, a int, b int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO f VALUES (1, 1, NULL);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE g (a int, b int, PRIMARY KEY (a, b));"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE f ADD CONSTRAINT f_fk FOREIGN KEY (a, b) REFERENCES g MATCH FULL;"#,
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "f" violates foreign key constraint "f_fk""#, detail: "MATCH FULL does not allow mixing of null and nonnull key values.", schema: "public", table: "f", constraint: "f_fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT id, pid, v, w2 FROM c ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("pid", INT4), Column("v", INT4), Column("w2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("5"), Null],
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
fn test_alter_table_adds_keys_over_existing_rows() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER TABLE adds primary keys and unique constraints to tables that have rows",
            set_up_script: &[
                "CREATE TABLE p1 (a INT, b INT);",
                "INSERT INTO p1 VALUES (1, 10), (NULL, 20);",
                "CREATE TABLE p2 (a INT, b INT);",
                "INSERT INTO p2 VALUES (1, 5), (2, 5);",
                "CREATE TABLE p3 (a INT, b INT);",
                "CREATE INDEX p3_b_idx ON p3 (b);",
                "INSERT INTO p3 VALUES (2, 20), (1, 10), (3, 30);",
                "CREATE TABLE p4 (a INT CONSTRAINT p4_a_nn NOT NULL NO INHERIT);",
                "CREATE TABLE p5 (a INT, b INT);",
                "INSERT INTO p5 VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE p1 ADD PRIMARY KEY (a);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"column "a" of relation "p1" contains null values"#, schema: "public", table: "p1", column: "a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p2 ADD UNIQUE (b);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"could not create unique index "p2_b_key""#, detail: "Key (b)=(5) is duplicated.", schema: "public", table: "p2", constraint: "p2_b_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p2 ADD UNIQUE (a);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p2 VALUES (2, 6);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "p2_a_key""#, detail: "Key (a)=(2) already exists.", schema: "public", table: "p2", constraint: "p2_a_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p3 ADD PRIMARY KEY (a);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM p3 WHERE b = 30;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM p3 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p4 ADD PRIMARY KEY (a);",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot create primary key on column "a""#, detail: r#"The constraint "p4_a_nn" on column "a" of table "p4", marked NO INHERIT, is incompatible with a primary key."#, hint: "You might need to make the existing constraint inheritable using ALTER TABLE ... ALTER CONSTRAINT ... INHERIT.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p5 ADD PRIMARY KEY (a, b);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p5 VALUES (1, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p5 VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM p5;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("1")],
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
fn test_existing_constraints_proving_new_ones() {
    run_scripts(&[
        ScriptTest {
            name: "existing constraints that prove a new constraint spare the check of the table's rows",
            set_up_script: &[
                "CREATE TABLE nn (a INT, b INT, CONSTRAINT a_present CHECK (a IS NOT NULL));",
                "INSERT INTO nn VALUES (1, NULL);",
                "CREATE TABLE pp (a INT) PARTITION BY RANGE (a);",
                "CREATE TABLE pp1 (a INT NOT NULL, CONSTRAINT in_range CHECK (a >= 0 AND a < 10));",
                "CREATE TABLE pp_def PARTITION OF pp DEFAULT;",
                "CREATE TABLE pp2 (a INT);",
                "SET client_min_messages = debug1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE nn ALTER COLUMN a SET NOT NULL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { severity: "DEBUG", code: "00000", message: r#"existing constraints on column "nn.a" are sufficient to prove that it does not contain nulls"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE nn ALTER COLUMN b SET NOT NULL;",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"column "b" of relation "nn" contains null values"#, schema: "public", table: "nn", column: "b", ..E }),
                    notices: &[Diagnostic { severity: "DEBUG", code: "00000", message: r#"verifying table "nn""#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pp ATTACH PARTITION pp1 FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { severity: "DEBUG", code: "00000", message: r#"partition constraint for table "pp1" is implied by existing constraints"#, ..N }, Diagnostic { severity: "DEBUG", code: "00000", message: r#"verifying table "pp_def""#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pp ATTACH PARTITION pp2 FOR VALUES FROM (10) TO (20);",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { severity: "DEBUG", code: "00000", message: r#"verifying table "pp2""#, ..N }, Diagnostic { severity: "DEBUG", code: "00000", message: r#"verifying table "pp_def""#, ..N }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_drop_primary_key() {
    run_scripts(&[
        ScriptTest {
            name: "dropping a primary key stores the table without its key",
            set_up_script: &[
                "CREATE TABLE p (a INT PRIMARY KEY, b INT);",
                "CREATE INDEX p_b ON p (b);",
                "INSERT INTO p VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE p DROP CONSTRAINT p_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (2, 30);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM p WHERE a = 2;",
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
                    query: "SELECT * FROM p ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("2"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM p WHERE b = 30;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ROLLBACK and ROLLBACK TO SAVEPOINT keep a dropped primary key's table",
            set_up_script: &[
                "CREATE TABLE q (a INT PRIMARY KEY, b INT);",
                "INSERT INTO q VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE q DROP CONSTRAINT q_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES (1, 9);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM q ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES (1, 9);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "q_pkey""#, detail: "Key (a)=(1) already exists.", schema: "public", table: "q", constraint: "q_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE q DROP CONSTRAINT q_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES (1, 9);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES (3, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM q ORDER BY a;",
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
            ],
            ..S
        },
        ScriptTest {
            name: "dropping a primary key's column drops the key",
            set_up_script: &[
                "CREATE TABLE q (a INT PRIMARY KEY, b INT);",
                "INSERT INTO q VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE q DROP COLUMN a;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM q ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
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
            name: "dropping a primary key's column with CASCADE drops the key and its foreign keys",
            set_up_script: &[
                "CREATE TABLE r (a INT PRIMARY KEY, b INT);",
                "CREATE TABLE rc (x INT REFERENCES r);",
                "INSERT INTO r VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE r DROP COLUMN a CASCADE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to constraint rc_x_fkey on table rc", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM r;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rc VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping a partitioned table's primary key drops each partition's",
            set_up_script: &[
                "CREATE TABLE pt (a INT, b INT, PRIMARY KEY (a)) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "INSERT INTO pt VALUES (1, 1), (11, 11);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE pt DROP CONSTRAINT pt_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES (1, 2), (11, 12);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("2")],
                            &[T("11"), T("11")],
                            &[T("11"), T("12")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "another session waits for an uncommitted primary key drop",
            set_up_script: &[
                "CREATE TABLE s (a INT PRIMARY KEY, b INT);",
                "INSERT INTO s VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE s DROP CONSTRAINT s_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("9")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO s VALUES (1, 9);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("9")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_table_drop_column_rows() {
    run_scripts(&[
        ScriptTest {
            name: "DROP COLUMN keeps the other columns of a keyed table's rows",
            set_up_script: &[
                "CREATE TABLE dc (id INT PRIMARY KEY, a TEXT, b INT, c TEXT);",
                "INSERT INTO dc VALUES (1, 'one', 10, 'x'), (2, 'two', 20, 'y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE dc DROP COLUMN a;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dc ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("10"), T("x")],
                            &[T("2"), T("20"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dc VALUES (3, 30, 'z');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dc ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("10"), T("x")],
                            &[T("2"), T("20"), T("y")],
                            &[T("3"), T("30"), T("z")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP COLUMN keeps the other columns of a keyless table's rows",
            set_up_script: &[
                "CREATE TABLE kl (a TEXT, b INT);",
                "INSERT INTO kl VALUES ('p', 1), ('q', 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE kl DROP COLUMN a;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM kl ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("b", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP COLUMN on a partitioned table keeps its partitions' rows",
            set_up_script: &[
                "CREATE TABLE pt (id INT, a TEXT, b INT) PARTITION BY RANGE (id);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "INSERT INTO pt VALUES (1, 'a1', 100), (11, 'a11', 1100);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE pt DROP COLUMN a;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
                            &[T("11"), T("1100")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP COLUMN on an inheritance parent keeps its children's rows",
            set_up_script: &[
                "CREATE TABLE par (id INT PRIMARY KEY, a TEXT, b INT);",
                "CREATE TABLE chi (c TEXT) INHERITS (par);",
                "INSERT INTO par VALUES (1, 'pa', 1);",
                "INSERT INTO chi VALUES (2, 'ca', 2, 'cc');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE par DROP COLUMN a;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM par ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM chi ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("2"), T("2"), T("cc")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP COLUMN CASCADE of an indexed column keeps the table's other indexes",
            set_up_script: &[
                "CREATE TABLE ix (id INT PRIMARY KEY, a TEXT, b INT);",
                "CREATE INDEX ix_a ON ix (a);",
                "CREATE INDEX ix_b ON ix (b);",
                "INSERT INTO ix VALUES (1, 'x', 5), (2, 'y', 6);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ix DROP COLUMN a CASCADE;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ix WHERE b = 6;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ix ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("5")],
                            &[T("2"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP COLUMN and ADD COLUMN with a default in one ALTER TABLE",
            set_up_script: &[
                "CREATE TABLE da (id INT PRIMARY KEY, a TEXT, b INT);",
                "INSERT INTO da VALUES (1, 'x', 5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE da DROP COLUMN a, ADD COLUMN d INT DEFAULT 7;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM da;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), T("5"), T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP COLUMN rolls back with its transaction",
            set_up_script: &[
                "CREATE TABLE rb (id INT PRIMARY KEY, a TEXT, b INT);",
                "INSERT INTO rb VALUES (1, 'x', 5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rb DROP COLUMN a;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rb;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("5")],
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
                    query: "SELECT * FROM rb;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", TEXT), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("x"), T("5")],
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
fn test_alter_column_type() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER COLUMN TYPE converts values and rebuilds the defaults, indexes, constraints, and comments on the columns",
            set_up_script: &[
                "CREATE TABLE p (id INT PRIMARY KEY, n TEXT UNIQUE);",
                "CREATE TABLE c (id INT PRIMARY KEY, pid INT REFERENCES p, v INT CHECK (v > 0), d INT DEFAULT 5, w VARCHAR(10));",
                "CREATE INDEX c_w ON c (w) WHERE w <> 'z';",
                "INSERT INTO p VALUES (1, 'a'), (2, 'b');",
                "INSERT INTO c VALUES (10, 1, 3, 7, 'x'), (11, 2, 4, 8, 'yy');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COMMENT ON CONSTRAINT c_v_check ON c IS 'v positive';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON INDEX c_w IS 'w index';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ALTER COLUMN id TYPE BIGINT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c ALTER COLUMN pid TYPE BIGINT, ALTER COLUMN v TYPE NUMERIC, ALTER COLUMN d TYPE TEXT, ALTER COLUMN w TYPE TEXT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'c'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("format_type", TEXT)],
                        rows: &[
                            &[T("id"), T("integer")],
                            &[T("pid"), T("bigint")],
                            &[T("v"), T("numeric")],
                            &[T("d"), T("text")],
                            &[T("w"), T("text")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_expr(adbin, adrelid) FROM pg_attrdef WHERE adrelid = 'c'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_expr", TEXT)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid IN ('p'::regclass, 'c'::regclass) ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("c_id_not_null"), T("NOT NULL id")],
                            &[T("c_pid_fkey"), T("FOREIGN KEY (pid) REFERENCES p(id)")],
                            &[T("c_pkey"), T("PRIMARY KEY (id)")],
                            &[T("c_v_check"), T("CHECK ((v > (0)::numeric))")],
                            &[T("p_id_not_null"), T("NOT NULL id")],
                            &[T("p_n_key"), T("UNIQUE (n)")],
                            &[T("p_pkey"), T("PRIMARY KEY (id)")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass, pg_get_indexdef(indexrelid) FROM pg_index WHERE indrelid IN ('p'::regclass, 'c'::regclass) ORDER BY 1::text;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", REGCLASS), Column("pg_get_indexdef", TEXT)],
                        rows: &[
                            &[T("p_n_key"), T("CREATE UNIQUE INDEX p_n_key ON public.p USING btree (n)")],
                            &[T("c_pkey"), T("CREATE UNIQUE INDEX c_pkey ON public.c USING btree (id)")],
                            &[T("p_pkey"), T("CREATE UNIQUE INDEX p_pkey ON public.p USING btree (id)")],
                            &[T("c_w"), T("CREATE INDEX c_w ON public.c USING btree (w) WHERE (w <> 'z'::text)")],
                        ],
                        tag: "SELECT 4",
                    },
                    skip: Some("ORDER BY a constant leaves the rows' order to the plan, and an Index Only Scan of pg_index_indrelid_index returns indexrelid, so Doltgrust reads them in indrelid order (the owner, 2026-11-01)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description(oid, 'pg_constraint') FROM pg_constraint WHERE conname = 'c_v_check';",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("v positive")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('c_w'::regclass, 'pg_class');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("w index")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c VALUES (12, 3, 1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"c\" violates foreign key constraint \"c_pid_fkey\"", detail: "Key (pid)=(3) is not present in table \"p\".", schema: "public", table: "c", constraint: "c_pid_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c VALUES (12, 2, -1);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"c\" violates check constraint \"c_v_check\"", detail: "Failing row contains (12, 2, -1, 5, null).", schema: "public", table: "c", constraint: "c_v_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c (id, pid, v) VALUES (12, 2, 1.5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM c ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("pid", INT8), Column("v", NUMERIC), Column("d", TEXT), Column("w", TEXT)],
                        rows: &[
                            &[T("10"), T("1"), T("3"), T("7"), T("x")],
                            &[T("11"), T("2"), T("4"), T("8"), T("yy")],
                            &[T("12"), T("2"), T("1.5"), T("5"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM c WHERE w = 'yy';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM p WHERE id = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
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
            name: "ALTER COLUMN TYPE refuses what Postgres refuses",
            set_up_script: &[
                "CREATE TABLE t (a INT, b TEXT, c INT GENERATED ALWAYS AS (a * 2) STORED);",
                "INSERT INTO t VALUES (1, 'x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a TYPE INT, ALTER COLUMN a TYPE BIGINT;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot alter type of a column used by a generated column", detail: "Column \"a\" is used by generated column \"c\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN ctid TYPE INT;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot alter system column \"ctid\"", position: 28, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN nope TYPE INT;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"nope\" of relation \"t\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN b TYPE INT;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "column \"b\" cannot be cast automatically to type integer", hint: "You might need to specify \"USING b::integer\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN b TYPE POINT USING b;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "result of USING clause for column \"b\" cannot be cast automatically to type point", hint: "You might need to add an explicit cast.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN b TYPE nosuchtype;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "type \"nosuchtype\" does not exist", position: 35, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN b TYPE INT USING (SELECT 1);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot use subquery in transform expression", position: 45, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN b TYPE INT USING sum(a);",
                    expected: Expected::Error(Diagnostic { code: "42803", message: "aggregate functions are not allowed in transform expressions", position: 45, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a TYPE BIGINT;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot alter type of a column used by a generated column", detail: "Column \"a\" is used by generated column \"c\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN c TYPE BIGINT USING c;",
                    expected: Expected::Error(Diagnostic { code: "42611", message: "cannot specify USING when altering type of generated column", position: 28, detail: "Column \"c\" is a generated column.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW tv AS SELECT b FROM t;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN b TYPE VARCHAR(5);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot alter type of a column used by a view or rule", detail: "rule _RETURN on view tv depends on column \"b\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW tv;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION tf() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER trg BEFORE UPDATE OF b ON t FOR EACH ROW EXECUTE FUNCTION tf();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN b TYPE VARCHAR(5);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot alter type of a column used in a trigger definition", detail: "trigger trg on table t depends on column \"b\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER trg ON t;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pk (a INT, b INT) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pk ALTER COLUMN a TYPE BIGINT;",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "cannot alter column \"a\" because it is part of the partition key of relation \"pk\"", position: 29, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW v AS SELECT 1 AS x;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE v ALTER COLUMN x TYPE BIGINT;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "ALTER action ALTER COLUMN ... SET DATA TYPE cannot be performed on relation \"v\"", detail: "This operation is not supported for views.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN b TYPE VARCHAR(5) USING b;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", VARCHAR), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("x"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER COLUMN TYPE fails as an index build fails for values its conversion makes equal",
            set_up_script: &[
                "CREATE TABLE u (id INT PRIMARY KEY, s TEXT UNIQUE);",
                "INSERT INTO u VALUES (1, 'aa'), (2, 'b'), (3, 'cc');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE u ALTER COLUMN s TYPE INT USING length(s);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"u_s_key\"", detail: "Key (s)=(2) is duplicated.", schema: "public", table: "u", constraint: "u_s_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u ALTER COLUMN id TYPE INT USING id % 2;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"u_pkey\"", detail: "Key (id)=(1) is duplicated.", schema: "public", table: "u", constraint: "u_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("s", TEXT)],
                        rows: &[
                            &[T("1"), T("aa")],
                            &[T("2"), T("b")],
                            &[T("3"), T("cc")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u ALTER COLUMN id TYPE INT USING id * 10, ALTER COLUMN s TYPE VARCHAR(1) USING left(s, 1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("s", VARCHAR)],
                        rows: &[
                            &[T("10"), T("a")],
                            &[T("20"), T("b")],
                            &[T("30"), T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u ALTER COLUMN s TYPE TEXT, ALTER COLUMN s TYPE INT USING 1;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot alter type of column \"s\" twice", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER COLUMN TYPE changes inheritance children and partitions with their parent",
            set_up_script: &[
                "CREATE TABLE par (a INT, b TEXT);",
                "CREATE TABLE ch () INHERITS (par);",
                "INSERT INTO par VALUES (1, '1');",
                "INSERT INTO ch VALUES (2, '2');",
                "CREATE TABLE pt (k INT, v TEXT) PARTITION BY RANGE (k);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE INDEX pt_v ON pt (v);",
                "INSERT INTO pt VALUES (1, '5');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ONLY par ALTER COLUMN b TYPE INT USING b::int;",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "type of inherited column \"b\" must be changed in child tables too", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ch ALTER COLUMN b TYPE INT USING b::int;",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: "cannot alter inherited column \"b\"", position: 29, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE par ALTER COLUMN b TYPE INT USING b::int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, a, b, pg_typeof(b) FROM par ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", INT4), Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("par"), T("1"), T("1"), T("integer")],
                            &[T("ch"), T("2"), T("2"), T("integer")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt ALTER COLUMN v TYPE INT USING v::int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt WHERE v = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass, pg_get_indexdef(indexrelid) FROM pg_index WHERE indrelid IN ('pt'::regclass, 'pt1'::regclass) ORDER BY 1::text;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", REGCLASS), Column("pg_get_indexdef", TEXT)],
                        rows: &[
                            &[T("pt_v"), T("CREATE INDEX pt_v ON ONLY public.pt USING btree (v)")],
                            &[T("pt1_v_idx"), T("CREATE INDEX pt1_v_idx ON public.pt1 USING btree (v)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SET EXPRESSION on a stored generated column writes its new values and rebuilds its index",
            set_up_script: &[
                "CREATE TABLE g (id INT PRIMARY KEY, a INT, b INT GENERATED ALWAYS AS (a * 2) STORED);",
                "CREATE INDEX g_b ON g (b);",
                "INSERT INTO g VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE g ALTER COLUMN b SET EXPRESSION AS (a * 3);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM g ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("3")],
                            &[T("2"), T("2"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM g WHERE b = 6;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_indexdef('g_b'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_indexdef", TEXT)],
                        rows: &[
                            &[T("CREATE INDEX g_b ON public.g USING btree (b)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE g ALTER COLUMN a TYPE BIGINT;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot alter type of a column used by a generated column", detail: "Column \"a\" is used by generated column \"b\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE g ALTER COLUMN b TYPE BIGINT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b, pg_typeof(b) FROM g WHERE b = 6;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT8), Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("2"), T("6"), T("bigint")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER COLUMN TYPE fires table_rewrite only when its conversion can change values",
            set_up_script: &[
                "CREATE FUNCTION rw() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'rewrite % reason %', pg_event_trigger_table_rewrite_oid()::regclass, pg_event_trigger_table_rewrite_reason(); END $$;",
                "CREATE EVENT TRIGGER rwt ON table_rewrite EXECUTE FUNCTION rw();",
                "CREATE TABLE e (a INT PRIMARY KEY, b VARCHAR(5), c TIMESTAMP);",
                "INSERT INTO e VALUES (1, 'x', '2020-01-01 10:00');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE e ALTER COLUMN b TYPE VARCHAR(10);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE e ALTER COLUMN b TYPE TEXT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE e ALTER COLUMN a TYPE BIGINT;",
                    notices: &[Diagnostic { code: "00000", message: "rewrite e reason 4", ..N }],
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET timezone = 'UTC';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE e ALTER COLUMN c TYPE TIMESTAMPTZ;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET timezone = 'America/New_York';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE e ALTER COLUMN c TYPE TIMESTAMP;",
                    notices: &[Diagnostic { code: "00000", message: "rewrite e reason 4", ..N }],
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM e;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT8), Column("b", TEXT), Column("c", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("x"), T("2020-01-01 05:00:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER COLUMN TYPE rolls back with its transaction and savepoint",
            set_up_script: &[
                "CREATE TABLE r (id INT PRIMARY KEY, s TEXT);",
                "INSERT INTO r VALUES (1, '10'), (2, '20');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r ALTER COLUMN s TYPE INT USING s::int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s + 1 FROM r ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("11")],
                            &[T("21")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT sp;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE r ALTER COLUMN id TYPE NUMERIC;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO sp;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, pg_typeof(id), s FROM r ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("pg_typeof", REGTYPE), Column("s", INT4)],
                        rows: &[
                            &[T("1"), T("integer"), T("10")],
                            &[T("2"), T("integer"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, s, pg_typeof(s) FROM r ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("s", TEXT), Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("1"), T("10"), T("text")],
                            &[T("2"), T("20"), T("text")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SET DEFAULT and ALTER COLUMN TYPE replace a default without reporting its drop to sql_drop triggers",
            set_up_script: &[
                "CREATE FUNCTION dr() RETURNS event_trigger LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN FOR r IN SELECT * FROM pg_event_trigger_dropped_objects() LOOP RAISE NOTICE 'dropped % %', r.object_type, r.object_identity; END LOOP; END $$;",
                "CREATE EVENT TRIGGER drt ON sql_drop EXECUTE FUNCTION dr();",
                "CREATE TABLE e (a INT DEFAULT 1, b INT PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE e ALTER COLUMN a SET DEFAULT 2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE e ALTER COLUMN a TYPE BIGINT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE e ALTER COLUMN b TYPE BIGINT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE e ALTER COLUMN a DROP DEFAULT;",
                    notices: &[Diagnostic { code: "00000", message: "dropped default value for public.e.a", ..N }],
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_table_of() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER TABLE OF and NOT OF, and ALTER TYPE changing typed tables with CASCADE",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TYPE tt_t0 AS (z INET, x INT, y NUMERIC(8,2));",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE tt_t0 DROP ATTRIBUTE z;",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tt0 (x INT NOT NULL, y NUMERIC(8,2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tt1 (x INT, y BIGINT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tt2 (x INT, y NUMERIC(9,2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tt3 (y NUMERIC(8,2), x INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tt4 (x INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tt5 (x INT, y NUMERIC(8,2), z INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tt6 () INHERITS (tt0);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tt7 (x INT, q TEXT, y NUMERIC(8,2));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tt7 DROP q;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tt0 OF tt_t0;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tt1 OF tt_t0;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "table \"tt1\" has different type for column \"y\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tt2 OF tt_t0;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "table \"tt2\" has different type for column \"y\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tt3 OF tt_t0;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "table has column \"y\" where type requires \"x\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tt4 OF tt_t0;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "table is missing column \"y\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tt5 OF tt_t0;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "table has extra column \"z\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tt6 OF tt_t0;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "typed tables cannot inherit", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tt7 OF tt_t0;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE tt_t1 AS (x INT, y NUMERIC(8,2));",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tt7 OF tt_t1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, c.reloftype::regtype, (SELECT count(*) FROM pg_depend d WHERE d.objid = c.oid AND d.refclassid = 'pg_type'::regclass AND d.refobjid IN ('tt_t0'::regtype, 'tt_t1'::regtype)) AS deps FROM pg_class c WHERE c.relname IN ('tt0', 'tt7') ORDER BY c.relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("reloftype", REGTYPE), Column("deps", INT8)],
                        rows: &[
                            &[T("tt0"), T("tt_t0"), T("1")],
                            &[T("tt7"), T("tt_t1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tt7 NOT OF;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE tt7 NOT OF;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "\"tt7\" is not a typed table", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.reloftype, (SELECT count(*) FROM pg_depend d WHERE d.objid = c.oid AND d.refclassid = 'pg_type'::regclass) AS deps FROM pg_class c WHERE c.relname = 'tt7';",
                    expected: Expected::Rows {
                        columns: &[Column("reloftype", OID), Column("deps", INT8)],
                        rows: &[
                            &[T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE test_type2 AS (a INT, b TEXT);",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test_tbl2 OF test_type2;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test_tbl2_subclass () INHERITS (test_tbl2);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test_tbl2 VALUES (1, 'one');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE test_type2 ADD ATTRIBUTE c TEXT;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot alter type \"test_type2\" because it is the type of a typed table", hint: "Use ALTER ... CASCADE to alter the typed tables too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE test_type2 ADD ATTRIBUTE c TEXT CASCADE;",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE test_type2 ALTER ATTRIBUTE b TYPE VARCHAR;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot alter type \"test_type2\" because it is the type of a typed table", hint: "Use ALTER ... CASCADE to alter the typed tables too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE test_type2 ALTER ATTRIBUTE b TYPE VARCHAR CASCADE;",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE test_type2 DROP ATTRIBUTE b;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot alter type \"test_type2\" because it is the type of a typed table", hint: "Use ALTER ... CASCADE to alter the typed tables too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE test_type2 DROP ATTRIBUTE b CASCADE;",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE test_type2 RENAME ATTRIBUTE a TO aa;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot alter type \"test_type2\" because it is the type of a typed table", hint: "Use ALTER ... CASCADE to alter the typed tables too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE test_type2 RENAME ATTRIBUTE a TO aa CASCADE;",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, a.attname, a.atttypid::regtype FROM pg_attribute a JOIN pg_class c ON c.oid = a.attrelid WHERE c.relname IN ('test_type2', 'test_tbl2', 'test_tbl2_subclass') AND a.attnum > 0 AND NOT a.attisdropped ORDER BY c.relname, a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("attname", NAME), Column("atttypid", REGTYPE)],
                        rows: &[
                            &[T("test_tbl2"), T("aa"), T("integer")],
                            &[T("test_tbl2"), T("c"), T("text")],
                            &[T("test_tbl2_subclass"), T("aa"), T("integer")],
                            &[T("test_tbl2_subclass"), T("c"), T("text")],
                            &[T("test_type2"), T("aa"), T("integer")],
                            &[T("test_type2"), T("c"), T("text")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test_tbl2;",
                    expected: Expected::Rows {
                        columns: &[Column("aa", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test_tbl2_subclass VALUES (2, 'two');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test_tbl2 ORDER BY aa;",
                    expected: Expected::Rows {
                        columns: &[Column("aa", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("two")],
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
fn test_alter_table_missing_values() {
    run_scripts(&[ScriptTest {
        name: "ADD COLUMN keeps a constant default as the column's missing value, which rollbacks, DROP COLUMN, rewrites, and VACUUM FULL clear as in Postgres",
        assertions: &[
            ScriptTestAssertion {
                query: "CREATE TABLE mv (id int4 PRIMARY KEY, a int4);",
                expected: Expected::Tag("CREATE TABLE"),
                ..A
            },
            ScriptTestAssertion {
                query: "INSERT INTO mv VALUES (1, 1);",
                expected: Expected::Tag("INSERT 0 1"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TABLE mv ADD COLUMN b int4 DEFAULT 5, ADD COLUMN c text DEFAULT 'x y', ADD COLUMN d int4;",
                expected: Expected::Tag("ALTER TABLE"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT attname, atthasmissing, attmissingval FROM pg_attribute WHERE attrelid = 'mv'::regclass AND attnum > 0 ORDER BY attnum;",
                expected: Expected::Rows {
                    columns: &[Column("attname", NAME), Column("atthasmissing", BOOL), Column("attmissingval", ANYARRAY)],
                    rows: &[
                        &[T("id"), T("f"), Null],
                        &[T("a"), T("f"), Null],
                        &[T("b"), T("t"), T("{5}")],
                        &[T("c"), T("t"), T(r#"{"x y"}"#)],
                        &[T("d"), T("f"), Null],
                    ],
                    tag: "SELECT 5",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT * FROM mv;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4), Column("c", TEXT), Column("d", INT4)],
                    rows: &[
                        &[T("1"), T("1"), T("5"), T("x y"), Null],
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
                query: "ALTER TABLE mv ADD COLUMN e int4 DEFAULT 7;",
                expected: Expected::Tag("ALTER TABLE"),
                ..A
            },
            ScriptTestAssertion {
                query: "ROLLBACK;",
                expected: Expected::Tag("ROLLBACK"),
                ..A
            },
            ScriptTestAssertion {
                query: "BEGIN;",
                expected: Expected::Tag("BEGIN"),
                ..A
            },
            ScriptTestAssertion {
                query: "SAVEPOINT s;",
                expected: Expected::Tag("SAVEPOINT"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TABLE mv ADD COLUMN f int4 DEFAULT 8;",
                expected: Expected::Tag("ALTER TABLE"),
                ..A
            },
            ScriptTestAssertion {
                query: "ROLLBACK TO SAVEPOINT s;",
                expected: Expected::Tag("ROLLBACK"),
                ..A
            },
            ScriptTestAssertion {
                query: "COMMIT;",
                expected: Expected::Tag("COMMIT"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT attname, atthasmissing, attmissingval FROM pg_attribute WHERE attrelid = 'mv'::regclass AND attnum > 0 ORDER BY attnum;",
                expected: Expected::Rows {
                    columns: &[Column("attname", NAME), Column("atthasmissing", BOOL), Column("attmissingval", ANYARRAY)],
                    rows: &[
                        &[T("id"), T("f"), Null],
                        &[T("a"), T("f"), Null],
                        &[T("b"), T("t"), T("{5}")],
                        &[T("c"), T("t"), T(r#"{"x y"}"#)],
                        &[T("d"), T("f"), Null],
                    ],
                    tag: "SELECT 5",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TABLE mv DROP COLUMN b;",
                expected: Expected::Tag("ALTER TABLE"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT attname, atthasmissing, attmissingval FROM pg_attribute WHERE attrelid = 'mv'::regclass AND attnum > 0 ORDER BY attnum;",
                expected: Expected::Rows {
                    columns: &[Column("attname", NAME), Column("atthasmissing", BOOL), Column("attmissingval", ANYARRAY)],
                    rows: &[
                        &[T("id"), T("f"), Null],
                        &[T("a"), T("f"), Null],
                        &[T("........pg.dropped.3........"), T("f"), Null],
                        &[T("c"), T("t"), T(r#"{"x y"}"#)],
                        &[T("d"), T("f"), Null],
                    ],
                    tag: "SELECT 5",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TABLE mv ALTER COLUMN a TYPE int8;",
                expected: Expected::Tag("ALTER TABLE"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT attname, atthasmissing, attmissingval FROM pg_attribute WHERE attrelid = 'mv'::regclass AND attnum > 0 ORDER BY attnum;",
                expected: Expected::Rows {
                    columns: &[Column("attname", NAME), Column("atthasmissing", BOOL), Column("attmissingval", ANYARRAY)],
                    rows: &[
                        &[T("id"), T("f"), Null],
                        &[T("a"), T("f"), Null],
                        &[T("........pg.dropped.3........"), T("f"), Null],
                        &[T("c"), T("f"), Null],
                        &[T("d"), T("f"), Null],
                    ],
                    tag: "SELECT 5",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TABLE mv2 (id int4 PRIMARY KEY);",
                expected: Expected::Tag("CREATE TABLE"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TABLE mv2 ADD COLUMN g int4 DEFAULT 9;",
                expected: Expected::Tag("ALTER TABLE"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT attname, atthasmissing, attmissingval FROM pg_attribute WHERE attrelid = 'mv2'::regclass AND attnum > 0 ORDER BY attnum;",
                expected: Expected::Rows {
                    columns: &[Column("attname", NAME), Column("atthasmissing", BOOL), Column("attmissingval", ANYARRAY)],
                    rows: &[
                        &[T("id"), T("f"), Null],
                        &[T("g"), T("t"), T("{9}")],
                    ],
                    tag: "SELECT 2",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "VACUUM FULL mv2;",
                expected: Expected::Tag("VACUUM"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT attname, atthasmissing, attmissingval FROM pg_attribute WHERE attrelid = 'mv2'::regclass AND attnum > 0 ORDER BY attnum;",
                expected: Expected::Rows {
                    columns: &[Column("attname", NAME), Column("atthasmissing", BOOL), Column("attmissingval", ANYARRAY)],
                    rows: &[
                        &[T("id"), T("f"), Null],
                        &[T("g"), T("f"), Null],
                    ],
                    tag: "SELECT 2",
                },
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_alter_table_reads_rows_committed_after_a_repeatable_read_snapshot() {
    run_scripts(&[
        ScriptTest {
            name: "a REPEATABLE READ transaction's ALTER TABLE rewrite keeps, and its new CHECK constraint checks, the rows another client committed after its snapshot, as Postgres' ATRewriteTable reads with GetLatestSnapshot",
            set_up_script: &[
                "CREATE TABLE la (id INT PRIMARY KEY, v INT)",
                "INSERT INTO la VALUES (1, 10)",
                "CREATE TABLE lc (id INT PRIMARY KEY, v INT)",
                "INSERT INTO lc VALUES (1, 10)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM lc",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO la VALUES (2, 20)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO lc VALUES (2, 20)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ALTER TABLE la ALTER COLUMN v TYPE BIGINT",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM la",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SAVEPOINT s",
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ALTER TABLE lc ADD CONSTRAINT lc_small CHECK (v < 15)",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "check constraint \"lc_small\" of relation \"lc\" is violated by some row", schema: "public", table: "lc", constraint: "lc_small", ..E }),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK TO s",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT id, v FROM la ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT8)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_table_rewrites_keep_rows_committed_after_a_repeatable_read_snapshot() {
    run_scripts(&[
        ScriptTest {
            name: "a REPEATABLE READ transaction's SET UNLOGGED and its drops of a primary key and of a key column keep the rows another client committed after its snapshot",
            set_up_script: &[
                "CREATE TABLE lu (id INT PRIMARY KEY, v INT)",
                "INSERT INTO lu VALUES (1, 10)",
                "CREATE TABLE ld (id INT, v INT, CONSTRAINT ld_pkey PRIMARY KEY (id))",
                "INSERT INTO ld VALUES (1, 10)",
                "CREATE TABLE lk (id INT PRIMARY KEY, v INT)",
                "INSERT INTO lk VALUES (1, 10)",
                "CREATE TABLE lother (a INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM lother",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO lu VALUES (2, 20)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO ld VALUES (2, 20)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO lk VALUES (2, 20)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ALTER TABLE lu SET UNLOGGED",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM lu",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ALTER TABLE ld DROP CONSTRAINT ld_pkey",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM ld",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    skip: Some("dropping a primary key writes the table's rows again under their new keys as the transaction's own, so its snapshot sees the row committed after it, where Postgres leaves the rows in place"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ALTER TABLE lk DROP COLUMN id",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT count(*) FROM lk",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    skip: Some("dropping a key column writes the table's rows again under their new keys as the transaction's own, so its snapshot sees the row committed after it, where Postgres leaves the rows in place"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT id, v FROM lu ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT id, v FROM ld ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT v FROM lk ORDER BY v",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_table_pending_transforms() {
    run_scripts(&[
        ScriptTest {
            name: "instant ADD COLUMN, DROP COLUMN, SET STORAGE, and toast_tuple_target on a table with a primary key leave its old rows to every reader",
            set_up_script: &[
                "CREATE TABLE pt (id int PRIMARY KEY, a int, b text);",
                "INSERT INTO pt VALUES (1, 10, 'x'), (2, 20, 'y'), (3, 30, 'z');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE pt ADD COLUMN c int DEFAULT 5;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("x"), T("5")],
                            &[T("2"), T("20"), T("y"), T("5")],
                            &[T("3"), T("30"), T("z"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt WHERE id = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("2"), T("20"), T("y"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET a = a + 1 WHERE id = 1 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("x"), T("5")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM pt WHERE id = 3 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("3"), T("30"), T("z"), T("5")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt WHERE id = 2 FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("2"), T("20"), T("y"), T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES (2, 0, 'n') ON CONFLICT (id) DO UPDATE SET b = excluded.b || pt.c RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("2"), T("20"), T("n5"), T("5")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES (2, 0, 'dup');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"pt_pkey\"", detail: "Key (id)=(2) already exists.", schema: "public", table: "pt", constraint: "pt_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX pt_c_unique ON pt (c);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"pt_c_unique\"", detail: "Key (c)=(5) is duplicated.", schema: "public", table: "pt", constraint: "pt_c_unique", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX pt_c ON pt (c);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c FROM pt WHERE c = 5 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("5")],
                            &[T("2"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt DROP COLUMN a;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("x"), T("5")],
                            &[T("2"), T("n5"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt ADD COLUMN d text DEFAULT 'dd', DROP COLUMN b, ALTER COLUMN c SET STORAGE PLAIN;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4), Column("d", TEXT)],
                        rows: &[
                            &[T("1"), T("5"), T("dd")],
                            &[T("2"), T("5"), T("dd")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, atthasmissing, attmissingval FROM pg_attribute WHERE attrelid = 'pt'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("atthasmissing", BOOL), Column("attmissingval", ANYARRAY)],
                        rows: &[
                            &[T("id"), T("f"), Null],
                            &[T("........pg.dropped.2........"), T("f"), Null],
                            &[T("........pg.dropped.3........"), T("f"), Null],
                            &[T("c"), T("t"), T("{5}")],
                            &[T("d"), T("t"), T("{dd}")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt ADD COLUMN e int DEFAULT 9;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4), Column("d", TEXT), Column("e", INT4)],
                        rows: &[
                            &[T("1"), T("5"), T("dd"), T("9")],
                            &[T("2"), T("5"), T("dd"), T("9")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4), Column("d", TEXT)],
                        rows: &[
                            &[T("1"), T("5"), T("dd")],
                            &[T("2"), T("5"), T("dd")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt ADD COLUMN e int DEFAULT 9;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4), Column("d", TEXT)],
                        rows: &[
                            &[T("1"), T("5"), T("dd")],
                            &[T("2"), T("5"), T("dd")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pt VALUES (10, 7, 'new');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt ADD COLUMN g int DEFAULT 1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4), Column("d", TEXT), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("5"), T("dd"), T("1")],
                            &[T("2"), T("5"), T("dd"), T("1")],
                            &[T("10"), T("7"), T("new"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4), Column("d", TEXT), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("5"), T("dd"), T("1")],
                            &[T("2"), T("5"), T("dd"), T("1")],
                            &[T("10"), T("7"), T("new"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt ADD COLUMN f text DEFAULT repeat('v', 5000);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt ALTER COLUMN f SET STORAGE EXTERNAL;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt ALTER COLUMN f SET STORAGE MAIN;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, length(f), md5(f) FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("length", INT4), Column("md5", TEXT)],
                        rows: &[
                            &[T("1"), T("5000"), T("2af464993129d7ea88e904250441c791")],
                            &[T("2"), T("5000"), T("2af464993129d7ea88e904250441c791")],
                            &[T("10"), T("5000"), T("2af464993129d7ea88e904250441c791")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pt SET (toast_tuple_target = 128);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c, d, g, length(f) FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4), Column("d", TEXT), Column("g", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("1"), T("5"), T("dd"), T("1"), T("5000")],
                            &[T("2"), T("5"), T("dd"), T("1"), T("5000")],
                            &[T("10"), T("7"), T("new"), T("1"), T("5000")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pt SET f = 'short' WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c, d, g, f = 'short' AS short FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4), Column("d", TEXT), Column("g", INT4), Column("short", BOOL)],
                        rows: &[
                            &[T("1"), T("5"), T("dd"), T("1"), T("t")],
                            &[T("2"), T("5"), T("dd"), T("1"), T("f")],
                            &[T("10"), T("7"), T("new"), T("1"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM FULL pt;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c, d, g, length(f) FROM pt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4), Column("d", TEXT), Column("g", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("1"), T("5"), T("dd"), T("1"), T("5")],
                            &[T("2"), T("5"), T("dd"), T("1"), T("5000")],
                            &[T("10"), T("7"), T("new"), T("1"), T("5000")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, atthasmissing, attmissingval FROM pg_attribute WHERE attrelid = 'pt'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("atthasmissing", BOOL), Column("attmissingval", ANYARRAY)],
                        rows: &[
                            &[T("id"), T("f"), Null],
                            &[T("........pg.dropped.2........"), T("f"), Null],
                            &[T("........pg.dropped.3........"), T("f"), Null],
                            &[T("c"), T("f"), Null],
                            &[T("d"), T("f"), Null],
                            &[T("g"), T("f"), Null],
                            &[T("f"), T("f"), Null],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a REPEATABLE READ transaction's first read of a table another transaction added a column to reads its snapshot's rows with the new column",
            set_up_script: &[
                "CREATE TABLE prr (id int PRIMARY KEY, a int);",
                "INSERT INTO prr VALUES (1, 1), (2, 2);",
                "CREATE TABLE prr_other (x int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM prr_other;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE prr ADD COLUMN c int DEFAULT 5;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO prr VALUES (3, 3, 7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM prr ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("5")],
                            &[T("2"), T("2"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM prr ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("5")],
                            &[T("2"), T("2"), T("5")],
                            &[T("3"), T("3"), T("7")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "READ COMMITTED's recheck of a row of a table with a pending transform reads the newer version with its columns",
            set_up_script: &[
                "CREATE TABLE prc (id int PRIMARY KEY, a int);",
                "INSERT INTO prc VALUES (1, 1), (2, 2);",
                "ALTER TABLE prc ADD COLUMN c int DEFAULT 5;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE prc SET a = a + 1 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE prc SET a = a * 10 WHERE id <= 2 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("2"), T("20"), T("5")],
                            &[T("1"), T("20"), T("5")],
                        ],
                        tag: "UPDATE 2",
                    },
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM prc ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("20"), T("5")],
                            &[T("2"), T("20"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE INDEX in a REPEATABLE READ transaction indexes the rows of a column another transaction added after its snapshot",
            set_up_script: &[
                "CREATE TABLE pci (id int PRIMARY KEY, a int);",
                "INSERT INTO pci VALUES (1, 1);",
                "CREATE TABLE pci_other (x int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pci_other;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pci ADD COLUMN c int DEFAULT 5;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pci VALUES (2, 2, 6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX pci_c ON pci (c);",
                    expected: Expected::Tag("CREATE INDEX"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, c FROM pci WHERE c >= 5 ORDER BY c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("5")],
                            &[T("2"), T("6")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a table's page count and REPEATABLE sample stay as they are right after an instant ADD COLUMN once its rows are rewritten",
            set_up_script: &[
                "CREATE TABLE pc (id int PRIMARY KEY, v text);",
                "INSERT INTO pc SELECT i, repeat('x', i % 40) FROM generate_series(1, 3000) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE pc;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pc_before AS SELECT pg_relation_size('pc') AS size, (SELECT array_agg(id ORDER BY id) FROM pc TABLESAMPLE SYSTEM (20) REPEATABLE (3)) AS ids;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE pc ADD COLUMN c int DEFAULT 5;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE pc_after AS SELECT pg_relation_size('pc') AS size, (SELECT array_agg(id ORDER BY id) FROM pc TABLESAMPLE SYSTEM (20) REPEATABLE (3)) AS ids;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.size = b.size AS same_size, a.ids = b.ids AS same_sample FROM pc_after a, pc_before b;",
                    expected: Expected::Rows {
                        columns: &[Column("same_size", BOOL), Column("same_sample", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("a table's page count and REPEATABLE samples move once, at an instant ALTER's commit, where Postgres keeps them (Appendix C)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE pc;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_relation_size('pc') = size AS same_size, (SELECT array_agg(id ORDER BY id) FROM pc TABLESAMPLE SYSTEM (20) REPEATABLE (3)) = ids AS same_sample FROM pc_after;",
                    expected: Expected::Rows {
                        columns: &[Column("same_size", BOOL), Column("same_sample", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(c) FROM pc;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("3000"), T("15000")],
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
fn test_without_overlaps_key_after_instant_add_column() {
    run_scripts(&[
        ScriptTest {
            name: "a WITHOUT OVERLAPS key checks rows written before an instant ADD COLUMN",
            set_up_script: &[
                "CREATE TABLE wot (id INT4RANGE, valid_at DATERANGE, note TEXT, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));",
                "INSERT INTO wot VALUES ('[1,2)', '[2020-01-01,2021-01-01)', 'a'), ('[2,3)', '[2020-01-01,2021-01-01)', 'b');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE wot ADD COLUMN extra INT DEFAULT 7;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wot VALUES ('[1,2)', '[2020-06-01,2020-07-01)', 'c', 8);",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"wot_pkey\"", detail: "Key (id, valid_at)=([1,2), [2020-06-01,2020-07-01)) conflicts with existing key (id, valid_at)=([1,2), [2020-01-01,2021-01-01)).", schema: "public", table: "wot", constraint: "wot_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wot VALUES ('[1,2)', '[2021-01-01,2022-01-01)', 'c', 8);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE wot SET valid_at = '[2020-06-01,2021-06-01)' WHERE note = 'b';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE wot SET id = '[1,2)' WHERE note = 'b';",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"wot_pkey\"", detail: "Key (id, valid_at)=([1,2), [2020-06-01,2021-06-01)) conflicts with existing key (id, valid_at)=([1,2), [2020-01-01,2021-01-01)).", schema: "public", table: "wot", constraint: "wot_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE wot DROP COLUMN note;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO wot VALUES ('[2,3)', '[2020-12-01,2021-02-01)', 9);",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"wot_pkey\"", detail: "Key (id, valid_at)=([2,3), [2020-12-01,2021-02-01)) conflicts with existing key (id, valid_at)=([2,3), [2020-06-01,2021-06-01)).", schema: "public", table: "wot", constraint: "wot_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM wot ORDER BY id, valid_at;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4RANGE), Column("valid_at", DATERANGE), Column("extra", INT4)],
                        rows: &[
                            &[T("[1,2)"), T("[2020-01-01,2021-01-01)"), T("7")],
                            &[T("[1,2)"), T("[2021-01-01,2022-01-01)"), T("8")],
                            &[T("[2,3)"), T("[2020-06-01,2021-06-01)"), T("7")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_table_set_without_oids() {
    run_scripts(&[
        ScriptTest {
            name: "SET WITHOUT OIDS does nothing, for the tables it applies to",
            set_up_script: &[
                "CREATE TABLE so (a int);",
                "CREATE VIEW sov AS SELECT * FROM so;",
                "CREATE TABLE sop (a int) PARTITION BY RANGE (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE so SET WITHOUT OIDS;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE sop SET WITHOUT OIDS;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE sov SET WITHOUT OIDS;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"ALTER action SET WITHOUT OIDS cannot be performed on relation "sov""#, detail: "This operation is not supported for views.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE IF EXISTS nope SET WITHOUT OIDS;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "00000", message: r#"relation "nope" does not exist, skipping"#, ..N }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_table_set_access_method() {
    run_scripts(&[
        ScriptTest {
            name: "SET ACCESS METHOD gives a table its access method and new relfilenodes when the method changes",
            set_up_script: &[
                "CREATE ACCESS METHOD heap2 TYPE TABLE HANDLER heap_tableam_handler;",
                "CREATE TABLE amt (a int PRIMARY KEY, b int);",
                "CREATE INDEX amt_b ON amt (b);",
                "INSERT INTO amt SELECT g, g FROM generate_series(1, 10) g;",
                "CREATE VIEW amv AS SELECT 1;",
                "CREATE TABLE amp (a int) PARTITION BY RANGE (a);",
                "CREATE MATERIALIZED VIEW amm AS SELECT 1 AS a;",
                "CREATE FUNCTION rw() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'rewrite % reason %', pg_event_trigger_table_rewrite_oid()::regclass, pg_event_trigger_table_rewrite_reason(); END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE before AS SELECT relname, relfilenode FROM pg_class WHERE relname LIKE 'amt%';",
                    expected: Expected::Tag("SELECT 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE amt SET ACCESS METHOD heap;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, c.relfilenode = b.relfilenode AS same FROM pg_class c JOIN before b USING (relname) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("same", BOOL)],
                        rows: &[
                            &[T("amt"), T("t")],
                            &[T("amt_b"), T("t")],
                            &[T("amt_pkey"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE amt SET ACCESS METHOD heap2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, c.relfilenode = b.relfilenode AS same FROM pg_class c JOIN before b USING (relname) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("same", BOOL)],
                        rows: &[
                            &[T("amt"), T("f")],
                            &[T("amt_b"), T("f")],
                            &[T("amt_pkey"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, (SELECT amname FROM pg_am WHERE oid = relam) FROM pg_class WHERE relname = 'amt';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("amname", NAME)],
                        rows: &[
                            &[T("amt"), T("heap2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(a) FROM amt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("10"), T("55")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) AS dependent, deptype FROM pg_depend WHERE refclassid = 'pg_am'::regclass AND refobjid = (SELECT oid FROM pg_am WHERE amname = 'heap2');",
                    expected: Expected::Rows {
                        columns: &[Column("dependent", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("table amt"), T("n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ACCESS METHOD heap2;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop access method heap2 because other objects depend on it", detail: "table amt depends on access method heap2", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE amt SET ACCESS METHOD heap2, SET ACCESS METHOD heap;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, (SELECT amname FROM pg_am WHERE oid = relam) FROM pg_class WHERE relname = 'amt';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("amname", NAME)],
                        rows: &[
                            &[T("amt"), T("heap")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE amt SET ACCESS METHOD heap2, SET ACCESS METHOD heap2;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot have multiple SET ACCESS METHOD subcommands", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE amt SET ACCESS METHOD btree;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"access method "btree" is not of type TABLE"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE amt SET ACCESS METHOD nope;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"access method "nope" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE amv SET ACCESS METHOD heap2;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"ALTER action SET ACCESS METHOD cannot be performed on relation "amv""#, detail: "This operation is not supported for views.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE amp SET ACCESS METHOD heap2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, (SELECT amname FROM pg_am WHERE oid = relam) FROM pg_class WHERE relname = 'amp';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("amname", NAME)],
                        rows: &[
                            &[T("amp"), T("heap2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE amp1 PARTITION OF amp FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, (SELECT amname FROM pg_am WHERE oid = relam) FROM pg_class WHERE relname = 'amp1';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("amname", NAME)],
                        rows: &[
                            &[T("amp1"), T("heap2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE amp SET ACCESS METHOD DEFAULT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, relam FROM pg_class WHERE relname = 'amp';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("relam", OID)],
                        rows: &[
                            &[T("amp"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER MATERIALIZED VIEW amm SET ACCESS METHOD heap2;",
                    expected: Expected::Tag("ALTER MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, (SELECT amname FROM pg_am WHERE oid = relam) FROM pg_class WHERE relname = 'amm';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("amname", NAME)],
                        rows: &[
                            &[T("amm"), T("heap2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER rwt ON table_rewrite EXECUTE FUNCTION rw();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE amt SET ACCESS METHOD heap2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    notices: &[Diagnostic { code: "00000", message: "rewrite amt reason 8", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE amt SET ACCESS METHOD heap2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER rwt;",
                    expected: Expected::Tag("DROP EVENT TRIGGER"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dropped_columns_are_not_found_by_name() {
    run_scripts(&[
        ScriptTest {
            name: "a dropped column's placeholder name names no column in keys, indexes, renames, or foreign keys",
            set_up_script: &[
                "CREATE TABLE atacc1 (id INT PRIMARY KEY, a INT);",
                "CREATE TABLE atacc2 (id INT PRIMARY KEY);",
                "ALTER TABLE atacc1 DROP COLUMN a;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE atacc1 ADD UNIQUE (\"........pg.dropped.2........\");",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"........pg.dropped.2........\" named in key does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON atacc1 (\"........pg.dropped.2........\");",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"........pg.dropped.2........\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE atacc1 RENAME \"........pg.dropped.2........\" TO x;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"........pg.dropped.2........\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE atacc1 ADD FOREIGN KEY (\"........pg.dropped.2........\") REFERENCES atacc2 (id);",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"........pg.dropped.2........\" referenced in foreign key constraint does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE atacc2 ADD FOREIGN KEY (id) REFERENCES atacc1 (\"........pg.dropped.2........\");",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"........pg.dropped.2........\" referenced in foreign key constraint does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_table_rewrites_give_new_relfilenodes() {
    run_scripts(&[
        ScriptTest {
            name: "a rewrite of a table's rows gives the table and its indexes new relfilenodes, and adding columns without one does not",
            set_up_script: &[
                "CREATE TABLE rewrite_test (col TEXT);",
                "CREATE INDEX rewrite_test_col ON rewrite_test (col);",
                "INSERT INTO rewrite_test VALUES ('something'), (NULL);",
                "CREATE TABLE filenodes AS SELECT relname, relfilenode FROM pg_class WHERE relname LIKE 'rewrite\\_test%';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE rewrite_test ADD COLUMN empty1 TEXT, ADD COLUMN notempty1_norewrite INT DEFAULT 42;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, c.relfilenode <> f.relfilenode AS changed FROM pg_class c JOIN filenodes f USING (relname) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("changed", BOOL)],
                        rows: &[
                            &[T("rewrite_test"), T("f")],
                            &[T("rewrite_test_col"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rewrite_test ADD COLUMN notempty2_rewrite SERIAL, ADD COLUMN empty2 TEXT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, c.relfilenode <> f.relfilenode AS changed FROM pg_class c JOIN filenodes f USING (relname) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("changed", BOOL)],
                        rows: &[
                            &[T("rewrite_test"), T("t")],
                            &[T("rewrite_test_col"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM filenodes;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO filenodes SELECT relname, relfilenode FROM pg_class WHERE relname LIKE 'rewrite\\_test%';",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rewrite_test SET UNLOGGED;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, c.relfilenode <> f.relfilenode AS changed FROM pg_class c JOIN filenodes f USING (relname) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("changed", BOOL)],
                        rows: &[
                            &[T("rewrite_test"), T("t")],
                            &[T("rewrite_test_col"), T("t")],
                            &[T("rewrite_test_notempty2_rewrite_seq"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_type_using_reads_columns_dropped_or_retyped_in_the_same_statement() {
    run_scripts(&[
        ScriptTest {
            name: "a USING expression reads the old row, whatever the statement's other subcommands drop or change",
            set_up_script: &[
                "CREATE TABLE another (f1 INT, f2 TEXT, f3 TEXT);",
                "INSERT INTO another VALUES (1, 'one', 'uno'), (2, 'two', 'due'), (3, 'three', 'tre');",
                "CREATE TABLE keyed (id INT PRIMARY KEY, f1 INT, f2 TEXT, f3 TEXT);",
                "INSERT INTO keyed VALUES (1, 1, 'one', 'uno'), (2, 2, 'two', 'due');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE another ALTER f1 TYPE TEXT USING f2 || ' and ' || f3 || ' more', ALTER f2 TYPE BIGINT USING f1 * 10, DROP COLUMN f3;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM another ORDER BY f2;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", TEXT), Column("f2", INT8)],
                        rows: &[
                            &[T("one and uno more"), T("10")],
                            &[T("two and due more"), T("20")],
                            &[T("three and tre more"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE keyed DROP COLUMN f3, ALTER f1 TYPE TEXT USING f2 || ' and ' || f3;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyed ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("f1", TEXT), Column("f2", TEXT)],
                        rows: &[
                            &[T("1"), T("one and uno"), T("one")],
                            &[T("2"), T("two and due"), T("two")],
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
fn test_add_primary_key_using_index_with_a_column_rewrite() {
    run_scripts(&[
        ScriptTest {
            name: "ADD PRIMARY KEY USING INDEX in the statement that rewrites the key column keys the rewritten rows",
            set_up_script: &[
                "CREATE TABLE ataddindex (f1 INT);",
                "INSERT INTO ataddindex VALUES (42), (43);",
                "CREATE UNIQUE INDEX ataddindexi0 ON ataddindex (f1);",
                "CREATE TABLE plain (f1 INT);",
                "INSERT INTO plain VALUES (1), (2);",
                "CREATE UNIQUE INDEX plaini0 ON plain (f1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ataddindex ADD PRIMARY KEY USING INDEX ataddindexi0, ALTER f1 TYPE BIGINT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f1, pg_typeof(f1) FROM ataddindex ORDER BY f1;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT8), Column("pg_typeof", REGTYPE)],
                        rows: &[
                            &[T("42"), T("bigint")],
                            &[T("43"), T("bigint")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ataddindex VALUES (42);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"ataddindexi0\"", detail: "Key (f1)=(42) already exists.", schema: "public", table: "ataddindex", constraint: "ataddindexi0", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f1 FROM ataddindex WHERE f1 = 43;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT8)],
                        rows: &[
                            &[T("43")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE plain ADD PRIMARY KEY USING INDEX plaini0;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO plain VALUES (2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"plaini0\"", detail: "Key (f1)=(2) already exists.", schema: "public", table: "plain", constraint: "plaini0", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f1 FROM plain WHERE f1 = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4)],
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
    ]);
}

#[test]
fn test_rename_with_a_statement_of_another_relation_kind() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER INDEX renames a table and ALTER TABLE an index, and ALTER SEQUENCE refuses an index",
            set_up_script: &[
                "CREATE TABLE alter_idx_rename_test (a INT);",
                "CREATE INDEX alter_idx_rename_test_idx ON alter_idx_rename_test (a);",
                "CREATE TABLE alter_idx_rename_test_parted (a INT) PARTITION BY LIST (a);",
                "CREATE INDEX alter_idx_rename_test_parted_idx ON alter_idx_rename_test_parted (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX alter_idx_rename_test RENAME TO alter_idx_rename_test_2;",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX alter_idx_rename_test_parted RENAME TO alter_idx_rename_test_parted_2;",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'alter\\_idx%' ORDER BY relation::regclass::text COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("relation", REGCLASS), Column("mode", TEXT)],
                        rows: &[
                            &[T("alter_idx_rename_test_2"), T("AccessExclusiveLock")],
                            &[T("alter_idx_rename_test_parted_2"), T("AccessExclusiveLock")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE alter_idx_rename_test_idx RENAME TO alter_idx_rename_test_idx_2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE alter_idx_rename_test_parted_idx RENAME TO alter_idx_rename_test_parted_idx_2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'alter\\_idx%' ORDER BY relation::regclass::text COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("relation", REGCLASS), Column("mode", TEXT)],
                        rows: &[
                            &[T("alter_idx_rename_test_idx_2"), T("AccessExclusiveLock")],
                            &[T("alter_idx_rename_test_parted_idx_2"), T("AccessExclusiveLock")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SEQUENCE alter_idx_rename_test_idx_2 RENAME TO alter_idx_rename_test_idx_3;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "\"alter_idx_rename_test_idx_2\" is not a sequence", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_inheritance_children_after_waiting_for_a_concurrent_alter() {
    run_scripts(&[
        ScriptTest {
            name: "a query that waited for a child leaving the inheritance still reads the child it found",
            set_up_script: &[
                "CREATE TABLE p (a INTEGER);",
                "INSERT INTO p VALUES (1);",
                "CREATE TABLE c1 () INHERITS (p);",
                "INSERT INTO c1 VALUES (10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c1 NO INHERIT p;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(a) FROM p;",
                    expected: Expected::Rows { columns: &[Column("sum", INT8)], rows: &[&[T("11")]], tag: "SELECT 1" },
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(a) FROM p;",
                    expected: Expected::Rows { columns: &[Column("sum", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a query that waited for a child's column to change type fails to match it to the parent",
            set_up_script: &[
                "CREATE TABLE p (a INTEGER);",
                "INSERT INTO p VALUES (1);",
                "CREATE TABLE c1 () INHERITS (p);",
                "INSERT INTO c1 VALUES (10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c1 NO INHERIT p;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c1 ALTER COLUMN a TYPE FLOAT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(a) FROM p;",
                    expected: Expected::Error(Diagnostic { code: "42611", message: "attribute \"a\" of relation \"c1\" does not match parent's type", ..E }),
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(a) FROM p;",
                    expected: Expected::Rows { columns: &[Column("sum", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a query that waited for a child's rewrite reads its new rows",
            set_up_script: &[
                "CREATE TABLE p (a INTEGER);",
                "INSERT INTO p VALUES (1);",
                "CREATE TABLE c1 () INHERITS (p);",
                "INSERT INTO c1 VALUES (10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c1 NO INHERIT p;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c1 ALTER COLUMN a TYPE INTEGER USING a + 1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(a) FROM p;",
                    expected: Expected::Rows { columns: &[Column("sum", INT8)], rows: &[&[T("12")]], tag: "SELECT 1" },
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(a) FROM p;",
                    expected: Expected::Rows { columns: &[Column("sum", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a prepared query that waited for a child leaving the inheritance keeps the plan with that child",
            set_up_script: &[
                "CREATE TABLE p (a INTEGER);",
                "INSERT INTO p VALUES (1);",
                "CREATE TABLE c1 () INHERITS (p);",
                "INSERT INTO c1 VALUES (10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "PREPARE q AS SELECT sum(a) FROM p;",
                    expected: Expected::Tag("PREPARE"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE c1 NO INHERIT p;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE q;",
                    expected: Expected::Rows { columns: &[Column("sum", INT8)], rows: &[&[T("11")]], tag: "SELECT 1" },
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE q;",
                    expected: Expected::Rows { columns: &[Column("sum", INT8)], rows: &[&[T("11")]], tag: "SELECT 1" },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(a) FROM p;",
                    expected: Expected::Rows { columns: &[Column("sum", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a query that waited for a child's drop leaves the child out",
            set_up_script: &[
                "CREATE TABLE p (a INTEGER);",
                "INSERT INTO p VALUES (1);",
                "CREATE TABLE c1 () INHERITS (p);",
                "INSERT INTO c1 VALUES (10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE c1;",
                    expected: Expected::Tag("DROP TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(a) FROM p;",
                    expected: Expected::Rows { columns: &[Column("sum", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_ddl_with_the_two_built_in_tablespaces() {
    run_scripts(&[
        ScriptTest {
            name: "pg_default is accepted and pg_global refused by SET TABLESPACE, ALL IN TABLESPACE, CREATE ... TABLESPACE, and default_tablespace",
            set_up_script: &[
                "CREATE TABLE ts_t (a INT PRIMARY KEY);",
                "CREATE TABLE ts_p (a INT) PARTITION BY RANGE (a);",
                "CREATE MATERIALIZED VIEW ts_m AS SELECT 1 AS x;",
                "CREATE ROLE ts_r;",
                "ALTER TABLE ts_t OWNER TO ts_r;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ts_c (a INT) TABLESPACE pg_default;",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ts_i ON ts_t (a) TABLESPACE pg_default;",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ts_t SET TABLESPACE pg_default;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ts_t SET TABLESPACE pg_global;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "only shared relations can be placed in pg_global tablespace", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ts_t SET TABLESPACE nosuch;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"tablespace "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ts_t SET TABLESPACE pg_default, SET TABLESPACE pg_default;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "cannot have multiple SET TABLESPACE subcommands", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ts_p SET TABLESPACE pg_default;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ts_p SET TABLESPACE pg_global;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "only shared relations can be placed in pg_global tablespace", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX ts_t_pkey SET TABLESPACE pg_default;",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER MATERIALIZED VIEW ts_m SET TABLESPACE pg_default;",
                    expected: Expected::Tag("ALTER MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ALL IN TABLESPACE pg_default SET TABLESPACE pg_default;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ALL IN TABLESPACE pg_default SET TABLESPACE pg_global;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot move relations in to or out of pg_global tablespace", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX ALL IN TABLESPACE pg_default SET TABLESPACE pg_default NOWAIT;",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ALL IN TABLESPACE pg_default OWNED BY nosuchrole SET TABLESPACE pg_default;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"role "nosuchrole" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ts_t SET TABLESPACE pg_default, ADD COLUMN z INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ts_g (a INT) TABLESPACE pg_global;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "only shared relations can be placed in pg_global tablespace", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ts_n (a INT) TABLESPACE nosuch;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"tablespace "nosuch" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ts_pp (a INT) PARTITION BY LIST (a) TABLESPACE pg_default;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot specify default tablespace for partitioned relations", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ts_pc PARTITION OF ts_p FOR VALUES FROM (1) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ts_gi ON ts_t (a) TABLESPACE pg_global;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "only shared relations can be placed in pg_global tablespace", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ts_ctas TABLESPACE pg_default AS SELECT 1 AS x;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW ts_mv TABLESPACE pg_default AS SELECT 1 AS x;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ts_cons (a INT, CONSTRAINT ts_cons_u UNIQUE (a) USING INDEX TABLESPACE pg_global);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "only shared relations can be placed in pg_global tablespace", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET default_tablespace = nosuch;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for parameter "default_tablespace": "nosuch""#, detail: r#"Tablespace "nosuch" does not exist."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER ROLE ts_r SET default_tablespace = nosuch;",
                    expected: Expected::Tag("ALTER ROLE"),
                    notices: &[Diagnostic { code: "42704", message: r#"tablespace "nosuch" does not exist"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET default_tablespace = pg_global;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ts_dg (a INT);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "only shared relations can be placed in pg_global tablespace", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET default_tablespace = pg_default;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ts_dd (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ts_dp (a INT) PARTITION BY LIST (a);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot specify default tablespace for partitioned relations", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE ts_tmp (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET default_tablespace = '';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ts_de (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ts_r;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ts_t SET TABLESPACE pg_global;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for tablespace pg_global", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ts_t SET TABLESPACE pg_default;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname, reltablespace FROM pg_class WHERE relname LIKE 'ts\\_%' ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("reltablespace", OID)],
                        rows: &[
                            &[T("ts_c"), T("0")],
                            &[T("ts_ctas"), T("0")],
                            &[T("ts_dd"), T("0")],
                            &[T("ts_de"), T("0")],
                            &[T("ts_i"), T("0")],
                            &[T("ts_m"), T("0")],
                            &[T("ts_mv"), T("0")],
                            &[T("ts_p"), T("0")],
                            &[T("ts_pc"), T("0")],
                            &[T("ts_t"), T("0")],
                            &[T("ts_t_pkey"), T("0")],
                            &[T("ts_tmp"), T("0")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_table_key_changes_keep_ctids() {
    run_scripts(&[
        ScriptTest {
            name: "adding, dropping, and rebuilding a primary key, and changes Postgres makes without rewriting the heap, keep every row's ctid",
            set_up_script: &[
                "CREATE TABLE t (a INT, b VARCHAR(10), c TEXT);",
                "INSERT INTO t SELECT i, 'v' || i, repeat('x', 100) FROM generate_series(1, 300) i;",
                "DELETE FROM t WHERE a % 3 = 0;",
                "CREATE TABLE k (a INT, b TEXT, c TEXT);",
                "INSERT INTO k SELECT i, 'v' || i, repeat('x', 100) FROM generate_series(1, 300) i;",
                "DELETE FROM k WHERE a % 3 = 0;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t ADD PRIMARY KEY (a);"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a FROM t WHERE a IN (1, 2, 4, 299) ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                            &[T("(0,4)"), T("4")],
                            &[T("(5,9)"), T("299")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a FROM t WHERE ctid = '(0,4)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE ctid >= '(1,0)' AND ctid < '(3,0)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("77")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"REINDEX TABLE t;"#,
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a FROM t WHERE a IN (1, 2, 4, 299) ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                            &[T("(0,4)"), T("4")],
                            &[T("(5,9)"), T("299")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t DROP CONSTRAINT t_pkey;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a FROM t WHERE a IN (1, 2, 4, 299) ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                            &[T("(0,4)"), T("4")],
                            &[T("(5,9)"), T("299")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a FROM t WHERE ctid = '(5,9)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("299")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE UNIQUE INDEX t_a ON t (a);"#,
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t ADD PRIMARY KEY USING INDEX t_a;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a FROM t WHERE a IN (1, 2, 4, 299) ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                            &[T("(0,4)"), T("4")],
                            &[T("(5,9)"), T("299")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t SET (fillfactor = 50);"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t ALTER COLUMN b TYPE varchar(20);"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a FROM t WHERE a IN (1, 2, 4, 299) ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                            &[T("(0,4)"), T("4")],
                            &[T("(5,9)"), T("299")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t DROP COLUMN b;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a FROM t WHERE a IN (1, 2, 4, 299) ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                            &[T("(0,4)"), T("4")],
                            &[T("(5,9)"), T("299")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1000, 'n');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a FROM t WHERE a = 1000;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(5,11)"), T("1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE k DROP COLUMN b;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a FROM k WHERE a IN (1, 2, 4, 299) ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                            &[T("(0,4)"), T("4")],
                            &[T("(5,9)"), T("299")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_column_type_changes_without_a_rewrite_keep_ctids() {
    run_scripts(&[
        ScriptTest {
            name: "a column type change Postgres makes without rewriting the heap keeps every ctid, and an older snapshot's ctid lookup finds the row it saw",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, c TEXT);",
                "INSERT INTO t SELECT i, repeat('x', 100) FROM generate_series(1, 300) i;",
                "DELETE FROM t WHERE a % 3 = 0 AND a < 300;",
                "CREATE TABLE k (a INT, c TEXT);",
                "INSERT INTO k SELECT i, repeat('x', 100) FROM generate_series(1, 300) i;",
                "DELETE FROM k WHERE a % 3 = 0;",
                "CREATE TABLE other (x INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"BEGIN ISOLATION LEVEL REPEATABLE READ;"#,
                    expected: Expected::Tag("BEGIN"),
                    client: "old",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM other;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "old",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DELETE FROM t WHERE a = 300;"#,
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t ALTER COLUMN c TYPE VARCHAR;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a FROM t WHERE a IN (1, 2, 4, 299) ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                            &[T("(0,4)"), T("4")],
                            &[T("(5,9)"), T("299")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1000, 'n');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a FROM t WHERE a = 1000;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(5,11)"), T("1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT a FROM t WHERE ctid = '(5,10)';"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("300")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "old",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMIT;"#,
                    expected: Expected::Tag("COMMIT"),
                    client: "old",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM t WHERE ctid = '(5,10)';"#,
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
                    query: r#"ALTER TABLE k ALTER COLUMN c TYPE VARCHAR;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a FROM k WHERE a IN (1, 2, 4, 299) ORDER BY a;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                            &[T("(0,4)"), T("4")],
                            &[T("(5,9)"), T("299")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO k VALUES (1000, 'n');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT ctid, a FROM k WHERE a = 1000;"#,
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4)],
                        rows: &[
                            &[T("(5,11)"), T("1000")],
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
fn test_rewrites_place_rows_in_ctid_order() {
    run_scripts(&[
        ScriptTest {
            name: "a rewrite that moves rows places them in the old heap's ctid order",
            set_up_script: &[
                "CREATE TABLE rk (k INT PRIMARY KEY, v TEXT);",
                "INSERT INTO rk VALUES (5, 'e'), (1, 'a'), (3, 'c');",
                "CREATE TABLE rn (a INT, v TEXT);",
                "INSERT INTO rn SELECT g, md5(g::text) FROM generate_series(1, 300) g;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE rk ALTER COLUMN v TYPE VARCHAR(10) USING v || 'x';",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, k, v FROM rk ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("k", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("(0,1)"), T("5"), T("ex")],
                            &[T("(0,2)"), T("1"), T("ax")],
                            &[T("(0,3)"), T("3"), T("cx")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rn ALTER COLUMN a TYPE BIGINT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, a FROM rn WHERE a IN (1, 2, 3, 120, 300) ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT8)],
                        rows: &[
                            &[T("(0,1)"), T("1")],
                            &[T("(0,2)"), T("2")],
                            &[T("(0,3)"), T("3")],
                            &[T("(1,13)"), T("120")],
                            &[T("(2,86)"), T("300")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE rk SET UNLOGGED;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, k FROM rk ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("k", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("5")],
                            &[T("(0,2)"), T("1")],
                            &[T("(0,3)"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_type_errors_point_into_the_definitions_it_creates_again() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER COLUMN TYPE reports where a statistics expression it creates again fails",
            intent: "The statistics object's expression has no operator for the new type, and the error points into the definition Postgres creates the object from again.",
            set_up_script: &[
                "CREATE TABLE st (a INT, b INT);",
                "CREATE STATISTICS st_s ON (a + b), b FROM st;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE st ALTER COLUMN a TYPE TEXT;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: text + integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 40, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER COLUMN TYPE reports where an index expression it creates again fails",
            intent: "The index expression has no operator for the new type, and the error points into the definition Postgres creates the index from again.",
            set_up_script: &[
                "CREATE TABLE it (a INT, b INT);",
                "CREATE INDEX ON it ((a + b));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE it ALTER COLUMN a TYPE TEXT;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: text + integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 56, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER COLUMN TYPE reports where an index predicate it creates again fails",
            intent: "The index predicate has no operator for the new type, and the error points into the definition Postgres creates the index from again.",
            set_up_script: &[
                "CREATE TABLE ip (a INT, b INT);",
                "CREATE INDEX ON ip (b) WHERE a + b > 0;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ip ALTER COLUMN a TYPE TEXT;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: text + integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 62, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER COLUMN TYPE reports where an exclusion constraint's expression it creates again fails",
            intent: "The exclusion constraint's expression has no operator for the new type, and the error points into the definition Postgres creates the constraint from again.",
            set_up_script: &[
                "CREATE TABLE ex (a INT, b INT);",
                "ALTER TABLE ex ADD EXCLUDE USING btree ((a + b) WITH =);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ex ALTER COLUMN a TYPE TEXT;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: text + integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 76, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER COLUMN TYPE reports where an exclusion constraint's predicate it creates again fails",
            intent: "The exclusion constraint's predicate has no operator for the new type, and the error points into the definition Postgres creates the constraint from again.",
            set_up_script: &[
                "CREATE TABLE ew (a INT, b INT);",
                "ALTER TABLE ew ADD EXCLUDE USING btree (b WITH =) WHERE (a + b > 0);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ew ALTER COLUMN a TYPE TEXT;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: text + integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 90, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ALTER COLUMN TYPE reports no position where a CHECK constraint it creates again fails",
            intent: "The CHECK constraint's expression has no operator for the new type, and Postgres reports the error without a position.",
            set_up_script: &[
                "CREATE TABLE ck (a INT, b INT CHECK (b > 0), CHECK (a + b > 0));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ck ALTER COLUMN a TYPE TEXT;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: text + integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 0, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_table_cluster_on_locks_the_index() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER TABLE ... CLUSTER ON locks the index with the command's lock as well as the table",
            set_up_script: &[
                "CREATE TABLE clon (f1 INT PRIMARY KEY, f2 TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE clon CLUSTER ON clon_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relation::regclass, mode FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'relation' AND relation::regclass::text LIKE 'clon%' ORDER BY relation::regclass::text COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("relation", REGCLASS), Column("mode", TEXT)],
                        rows: &[
                            &[T("clon"), T("ShareUpdateExclusiveLock")],
                            &[T("clon_pkey"), T("ShareUpdateExclusiveLock")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
