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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_branch_queries.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_foreign_key_branch_tests() {
    run_scripts(&[
        ScriptTest {
            name: "create fk on branch",
            intent: "A foreign key created on a branch exists and is enforced only there.",
            set_up_script: &[
                "CREATE TABLE parent (id INT PRIMARY KEY, v1 INT, v2 INT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id INT PRIMARY KEY, v1 INT, v2 INT);",
                "SELECT dolt_commit('-Am', 'checkpoint enginetest database mydb');",
                "SELECT dolt_branch('b1');",
                "SELECT * FROM dolt_checkout('b1');",
                "ALTER TABLE child ADD CONSTRAINT fk_named FOREIGN KEY (v1) REFERENCES parent(v1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_named"), T("FOREIGN KEY (v1) REFERENCES parent(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_constraint shows the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (1, 1, 1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"child\" violates foreign key constraint \"fk_named\"", detail: "Key (v1)=(1) is not present in table \"parent\".", schema: "public", table: "child", constraint: "fk_named", ..E }),
                    dolt: Some(Dolt::Setup("Dolt's harness loads its parent_child fixture and commits it first; the fixture's index on parent (v1) is unique, since a Postgres foreign key needs a unique index where MySQL's takes a plain one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_constraint shows the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (1, 1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.child VALUES (1, 1, 1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"child\" violates foreign key constraint \"fk_named\"", detail: "Key (v1)=(1) is not present in table \"parent\".", schema: "public", table: "child", constraint: "fk_named", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Test foreign-key creation in non-default revision databases",
            intent: "A foreign key created on a checked-out branch is enforced there.",
            set_up_script: &[
                "CREATE TABLE ref_parent(pk int PRIMARY KEY);",
                "CREATE TABLE ref_child(pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'create tables');",
                "SELECT dolt_branch('revision_branch');",
                "SELECT * FROM dolt_checkout('revision_branch');",
                "ALTER TABLE ref_child ADD CONSTRAINT revision_fk FOREIGN KEY(pk) REFERENCES ref_parent(pk);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO ref_child VALUES(1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"ref_child\" violates foreign key constraint \"revision_fk\"", detail: "Key (pk)=(1) is not present in table \"ref_parent\".", schema: "public", table: "ref_child", constraint: "revision_fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ref_parent VALUES(1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ref_child VALUES(1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ref_child;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
            name: "create fk with branch checkout",
            intent: "A foreign key created on a branch checked out with dolt_checkout exists and is enforced only there.",
            set_up_script: &[
                "CREATE TABLE parent (id INT PRIMARY KEY, v1 INT, v2 INT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id INT PRIMARY KEY, v1 INT, v2 INT);",
                "SELECT dolt_commit('-Am', 'checkpoint enginetest database mydb');",
                "SELECT dolt_branch('b1');",
                "SELECT * FROM dolt_checkout('b1');",
                "ALTER TABLE child ADD CONSTRAINT fk_named FOREIGN KEY (v1) REFERENCES parent(v1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_named"), T("FOREIGN KEY (v1) REFERENCES parent(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_constraint shows the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (1, 1, 1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"child\" violates foreign key constraint \"fk_named\"", detail: "Key (v1)=(1) is not present in table \"parent\".", schema: "public", table: "child", constraint: "fk_named", ..E }),
                    dolt: Some(Dolt::Setup("Dolt's harness loads its parent_child fixture and commits it first; the fixture's index on parent (v1) is unique, since a Postgres foreign key needs a unique index where MySQL's takes a plain one")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_constraint shows the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (1, 1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create fk on branch not being used",
            intent: "A foreign key created through a branch's revision name exists and is enforced only on that branch.",
            set_up_script: &[
                "CREATE TABLE parent (id INT PRIMARY KEY, v1 INT, v2 INT);",
                "CREATE UNIQUE INDEX v1 ON parent (v1);",
                "CREATE INDEX v2 ON parent (v2);",
                "CREATE TABLE child (id INT PRIMARY KEY, v1 INT, v2 INT);",
                "SELECT dolt_commit('-Am', 'checkpoint enginetest database mydb');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/b1\".public.child ADD CONSTRAINT fk_named FOREIGN KEY (v1) REFERENCES parent(v1);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::Bug("Dolt main fails this too, adding a foreign key through a revision database's name")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM \"postgres/b1\".pg_catalog.pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("fk_named"), T("FOREIGN KEY (v1) REFERENCES parent(v1)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_constraint shows the table's foreign keys; Dolt main fails this too, adding a foreign key through a revision database's name")),
                    skip: Some("pg_get_constraintdef reads the catalog of the branch the session runs under, so a constraint only another branch has shows NULL through a revision-qualified pg_constraint"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.child VALUES (1, 1, 1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"child\" violates foreign key constraint \"fk_named\"", detail: "Key (v1)=(1) is not present in table \"parent\".", schema: "public", table: "child", constraint: "fk_named", ..E }),
                    dolt: Some(Dolt::Bug("Dolt main fails this too, adding a foreign key through a revision database's name")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 'child'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_constraint shows the table's foreign keys")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (1, 1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_view_branch_tests() {
    run_scripts(&[
        ScriptTest {
            name: "create view on branch",
            intent: "A view created on a branch exists only there.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "INSERT INTO t1 VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE VIEW v1 AS SELECT * FROM t1 WHERE a > 2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"v1\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.v1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create view on different branch",
            intent: "A view created through another branch's revision name exists only on that branch.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "INSERT INTO t1 VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE VIEW \"postgres/b1\".public.v1 AS SELECT * FROM t1 WHERE a > 2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v1;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"v1\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.v1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
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
fn test_ddl_branch_tests() {
    run_scripts(&[
        ScriptTest {
            name: "create table on branch",
            intent: "A table created on a branch exists only there.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "INSERT INTO t1 VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "SELECT * FROM dolt_checkout('b1');",
                "CREATE TABLE t2 (a int PRIMARY KEY, b int);",
                "INSERT INTO t2 VALUES (4, 4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t2\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table on different branch",
            intent: "A table created through another branch's revision name exists only on that branch.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "INSERT INTO t1 VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "CREATE TABLE \"postgres/b1\".public.t2 (a int PRIMARY KEY, b int);",
                "INSERT INTO \"postgres/b1\".public.t2 VALUES (4,4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t2\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table on different branch, autocommit off",
            intent: "A table created through another branch's revision name in a transaction exists only on that branch once committed.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "INSERT INTO t1 VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
                "BEGIN;",
                "CREATE TABLE \"postgres/b1\".public.t2 (a int PRIMARY KEY, b int);",
                "INSERT INTO \"postgres/b1\".public.t2 VALUES (4,4);",
                "COMMIT;",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t2\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter table on different branch, add column",
            intent: "A column added through another branch's revision name exists only on that branch.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "INSERT INTO t1 VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/b1\".public.t1 ADD COLUMN c int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("1"), Null],
                            &[T("2"), T("2"), Null],
                            &[T("3"), T("3"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
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
            name: "alter table on different branch, drop column",
            intent: "A column dropped through another branch's revision name is gone only on that branch.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "INSERT INTO t1 VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/b1\".public.t1 DROP COLUMN b;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
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
            name: "alter table on different branch, modify column",
            intent: "A column's type changed through another branch's revision name changes only on that branch.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "INSERT INTO t1 VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/b1\".public.t1 ALTER COLUMN b TYPE varchar(1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MODIFY COLUMN ... FIRST moves the column first, which Postgres can't do, so only the type changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", VARCHAR)],
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
                    query: "SELECT * FROM t1;",
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
            name: "alter table on different branch, create and drop index",
            intent: "An index created and dropped through another branch's revision name exists only on that branch while it lasts.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "INSERT INTO t1 VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX idx ON \"postgres/b1\".public.t1 (b);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM \"postgres/b1\".pg_catalog.pg_indexes WHERE tablename = 't1' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("idx")],
                            &[T("t1_pkey")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 't1' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("t1_pkey")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_indexes shows the table's indexes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX \"postgres/b1\".public.idx;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM \"postgres/b1\".pg_catalog.pg_indexes WHERE tablename = 't1' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("t1_pkey")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_indexes shows the table's indexes")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter table on different branch, add and drop constraint",
            intent: "A check constraint added and dropped through another branch's revision name is enforced only on that branch while it lasts.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b int);",
                "INSERT INTO t1 VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT dolt_commit('-Am', 'first commit');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/b1\".public.t1 ADD CONSTRAINT chk1 CHECK (b < 4);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM \"postgres/b1\".pg_catalog.pg_constraint WHERE conrelid = 't1'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("chk1"), T("CHECK ((b < 4))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_constraint shows the table's check constraints")),
                    skip: Some("pg_get_constraintdef reads the catalog of the branch the session runs under, so a constraint only another branch has shows NULL through a revision-qualified pg_constraint"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t1 VALUES (4, 4);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"t1\" violates check constraint \"chk1\"", detail: "Failing row contains (4, 4).", schema: "public", table: "t1", constraint: "chk1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 't1'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_constraint shows the table's check constraints")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (4, 4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE \"postgres/b1\".public.t1 DROP CONSTRAINT chk1;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM \"postgres/b1\".pg_catalog.pg_constraint WHERE conrelid = 't1'::regclass AND contype = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_constraint shows the table's check constraints")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
