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
fn test_set_constraints() {
    run_scripts(&[
        ScriptTest {
            name: "SET CONSTRAINTS",
            set_up_script: &[
                "CREATE TABLE parent_example (id INTEGER PRIMARY KEY, u INTEGER CONSTRAINT uniq_u UNIQUE, c INTEGER CONSTRAINT chk CHECK (c > 0));",
                "CREATE TABLE child_example (id INTEGER PRIMARY KEY, p2 INTEGER CONSTRAINT nd_fk REFERENCES parent_example(id));",
                "CREATE SCHEMA s2;",
                "CREATE TABLE s2.t2 (id INTEGER PRIMARY KEY);",
                "CREATE DOMAIN dom AS INTEGER CONSTRAINT dom_chk CHECK (VALUE > 0);",
                "CREATE TABLE named_pk_example (id INTEGER CONSTRAINT named_pk PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS ALL IMMEDIATE;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "SET CONSTRAINTS can only be used in transaction blocks", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS ALL DEFERRED;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "SET CONSTRAINTS can only be used in transaction blocks", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS nope IMMEDIATE;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"constraint "nope" does not exist"#, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "SET CONSTRAINTS can only be used in transaction blocks", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS ALL IMMEDIATE;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS ALL DEFERRED;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS nd_fk, parent_example_pkey, uniq_u, chk, dom_chk IMMEDIATE;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS ND_FK IMMEDIATE;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS public.nd_fk IMMEDIATE;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS postgres.public.nd_fk IMMEDIATE;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS s2.t2_pkey IMMEDIATE;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS t2_pkey IMMEDIATE;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"constraint "t2_pkey" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET CONSTRAINTS "ND_FK" IMMEDIATE;"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"constraint "ND_FK" does not exist"#, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "SET CONSTRAINTS can only be used in transaction blocks", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS public.t2_pkey IMMEDIATE;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"constraint "t2_pkey" does not exist"#, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "SET CONSTRAINTS can only be used in transaction blocks", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS nd_fk, nope IMMEDIATE;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"constraint "nope" does not exist"#, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "SET CONSTRAINTS can only be used in transaction blocks", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS nosuch.nd_fk IMMEDIATE;",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: r#"schema "nosuch" does not exist"#, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "SET CONSTRAINTS can only be used in transaction blocks", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS otherdb.public.nd_fk IMMEDIATE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cross-database references are not implemented: "otherdb.public.nd_fk""#, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "SET CONSTRAINTS can only be used in transaction blocks", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS nd_fk DEFERRED;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"constraint "nd_fk" is not deferrable"#, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "SET CONSTRAINTS can only be used in transaction blocks", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS dom_chk DEFERRED;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"constraint "dom_chk" is not deferrable"#, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "SET CONSTRAINTS can only be used in transaction blocks", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS nd_fk, nope DEFERRED;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"constraint "nd_fk" is not deferrable"#, ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "SET CONSTRAINTS can only be used in transaction blocks", ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = s2, public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS t2_pkey, nd_fk IMMEDIATE;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
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
                    query: "SET CONSTRAINTS named_pk IMMEDIATE;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS a.b.c.d IMMEDIATE;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "improper qualified name (too many dotted names): a.b.c.d", position: 17, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS ALL DEFERRED;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child_example VALUES (1, 99);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "child_example" violates foreign key constraint "nd_fk""#, detail: r#"Key (p2)=(99) is not present in table "parent_example"."#, schema: "public", table: "child_example", constraint: "nd_fk", ..E }),
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
        ScriptTest {
            name: "SET CONSTRAINTS with deferred foreign keys",
            set_up_script: &[
                "CREATE TABLE parent_example (id INTEGER PRIMARY KEY);",
                "CREATE TABLE child_example (id INTEGER PRIMARY KEY, parent_id INTEGER CONSTRAINT child_parent_fk REFERENCES parent_example(id) DEFERRABLE INITIALLY DEFERRED);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child_example VALUES (1, 10);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO parent_example VALUES (10);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "INSERT INTO child_example VALUES (2, 20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "child_example" violates foreign key constraint "child_parent_fk""#, detail: r#"Key (parent_id)=(20) is not present in table "parent_example"."#, schema: "public", table: "child_example", constraint: "child_parent_fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child_example VALUES (3, 30);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS child_parent_fk IMMEDIATE;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "child_example" violates foreign key constraint "child_parent_fk""#, detail: r#"Key (parent_id)=(30) is not present in table "parent_example"."#, schema: "public", table: "child_example", constraint: "child_parent_fk", ..E }),
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
                    query: "SET CONSTRAINTS ALL IMMEDIATE;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child_example VALUES (4, 40);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "child_example" violates foreign key constraint "child_parent_fk""#, detail: r#"Key (parent_id)=(40) is not present in table "parent_example"."#, schema: "public", table: "child_example", constraint: "child_parent_fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child_example;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("parent_id", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
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
                    query: "SET CONSTRAINTS child_parent_fk DEFERRED;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS child_parent_fk IMMEDIATE;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
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
        ScriptTest {
            name: "SET CONSTRAINTS with deferrable constraints",
            set_up_script: &[
                "CREATE TABLE p (id INTEGER PRIMARY KEY);",
                "CREATE TABLE fk_imm (id INTEGER PRIMARY KEY, pid INTEGER CONSTRAINT fk_imm_fk REFERENCES p(id) DEFERRABLE INITIALLY IMMEDIATE);",
                "CREATE TABLE u_def (id INTEGER PRIMARY KEY, v INTEGER CONSTRAINT u_def_u UNIQUE DEFERRABLE INITIALLY DEFERRED);",
                "CREATE TABLE pk_def (id INTEGER CONSTRAINT pk_def_pk PRIMARY KEY DEFERRABLE, v INTEGER);",
                "INSERT INTO u_def VALUES (1, 1), (2, 2);",
                "INSERT INTO pk_def VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fk_imm VALUES (1, 10);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "fk_imm" violates foreign key constraint "fk_imm_fk""#, detail: r#"Key (pid)=(10) is not present in table "p"."#, schema: "public", table: "fk_imm", constraint: "fk_imm_fk", ..E }),
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
                    query: "SET CONSTRAINTS fk_imm_fk DEFERRED;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fk_imm VALUES (1, 10);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (10);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "SET CONSTRAINTS ALL DEFERRED;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fk_imm VALUES (2, 20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fk_imm ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("pid", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
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
                    query: "UPDATE u_def SET v = 2 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE u_def SET v = 1 WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u_def ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("1")],
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
                    query: "UPDATE u_def SET v = 1 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "u_def_u""#, detail: "Key (v)=(1) already exists.", schema: "public", table: "u_def", constraint: "u_def_u", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE u_def SET v = 1 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS u_def_u IMMEDIATE;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "u_def_u""#, detail: "Key (v)=(1) already exists.", schema: "public", table: "u_def", constraint: "u_def_u", ..E }),
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
                    query: "SET CONSTRAINTS pk_def_pk DEFERRED;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pk_def SET id = 2 WHERE v = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pk_def SET id = 1 WHERE v = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pk_def ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("2"), T("1")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pk_def SET id = id + 10;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pk_def ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("12"), T("1")],
                            &[T("11"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "SET CONSTRAINTS with deferred parent deletions",
            set_up_script: &[
                "CREATE TABLE dp (id INTEGER PRIMARY KEY);",
                "CREATE TABLE dc (id INTEGER PRIMARY KEY, pid INTEGER CONSTRAINT dc_fk REFERENCES dp(id) DEFERRABLE INITIALLY DEFERRED);",
                "CREATE TABLE du (id INTEGER PRIMARY KEY, v INTEGER CONSTRAINT du_v UNIQUE DEFERRABLE);",
                "INSERT INTO dp VALUES (1);",
                "INSERT INTO dc VALUES (1, 1);",
                "INSERT INTO du VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dp WHERE id = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dp VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "DELETE FROM dp WHERE id = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"update or delete on table "dp" violates foreign key constraint "dc_fk" on table "dc""#, detail: r#"Key (id)=(1) is still referenced from table "dc"."#, schema: "public", table: "dc", constraint: "dc_fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dc VALUES (2, 5);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "dc" violates foreign key constraint "dc_fk""#, detail: r#"Key (pid)=(5) is not present in table "dp"."#, schema: "public", table: "dc", constraint: "dc_fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dc VALUES (3, 7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dc WHERE id = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dc ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("pid", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE du SET v = v + 1;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM du ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indimmediate FROM pg_index WHERE indexrelid = 'du_v'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indimmediate", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT constraint_name, is_deferrable, initially_deferred FROM information_schema.table_constraints WHERE table_name IN ('dc', 'du') AND constraint_type <> 'CHECK' ORDER BY constraint_name;",
                    expected: Expected::Rows {
                        columns: &[Column("constraint_name", NAME), Column("is_deferrable", VARCHAR), Column("initially_deferred", VARCHAR)],
                        rows: &[
                            &[T("dc_fk"), T("YES"), T("YES")],
                            &[T("dc_pkey"), T("NO"), T("NO")],
                            &[T("du_pkey"), T("NO"), T("NO")],
                            &[T("du_v"), T("YES"), T("NO")],
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
fn test_deferrable_unique_constraints() {
    run_scripts(&[
        ScriptTest {
            name: "deferrable unique and primary key constraints are checked when the statement or transaction ends",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY DEFERRABLE, b INT UNIQUE DEFERRABLE INITIALLY DEFERRED, c INT);",
                "INSERT INTO t VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t SET a = a + 1;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[&[T("2"), T("1"), T("1")], &[T("3"), T("2"), T("2")], &[T("4"), T("3"), T("3")]],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4, 9, 0);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t_pkey""#, detail: "Key (a)=(4) already exists.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (10, 2, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t_b_key""#, detail: "Key (b)=(2) already exists.", schema: "public", table: "t", constraint: "t_b_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (10, 2, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS t_b_key IMMEDIATE;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t_b_key""#, detail: "Key (b)=(2) already exists.", schema: "public", table: "t", constraint: "t_b_key", ..E }),
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
                    query: "INSERT INTO t VALUES (10, 2, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE a = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[&[T("2"), T("1"), T("1")], &[T("4"), T("3"), T("3")], &[T("10"), T("2"), T("0")]],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET CONSTRAINTS ALL DEFERRED;",
                    expected: Expected::Tag("SET CONSTRAINTS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 20, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET a = 5 WHERE b = 20;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t WHERE b = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[&[T("10"), T("2"), T("0")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4, 30, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t WHERE a = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[&[T("2")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ADD PRIMARY KEY DEFERRABLE over existing rows, identical rows, and NULLs",
            set_up_script: &[
                "CREATE TABLE u (a INT, b INT);",
                "INSERT INTO u VALUES (1, 1), (1, 2);",
                "CREATE TABLE k (a INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED);",
                "CREATE TABLE n (a INT UNIQUE DEFERRABLE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE u ADD PRIMARY KEY (a) DEFERRABLE;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"could not create unique index "u_pkey""#, detail: "Key (a)=(1) is duplicated.", schema: "public", table: "u", constraint: "u_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM u WHERE b = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u ADD PRIMARY KEY (a) DEFERRABLE INITIALLY DEFERRED;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO u VALUES (1, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "u_pkey""#, detail: "Key (a)=(1) already exists.", schema: "public", table: "u", constraint: "u_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO k VALUES (1), (1);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "k_pkey""#, detail: "Key (a)=(1) already exists.", schema: "public", table: "k", constraint: "k_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO k VALUES (1), (1);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM k;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO k VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM k;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[&[T("1")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE f (a INT REFERENCES k);",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot use a deferrable primary key for referenced table "k""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO n VALUES (NULL), (NULL), (1);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO n VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "n_a_key""#, detail: "Key (a)=(1) already exists.", schema: "public", table: "n", constraint: "n_a_key", ..E }),
                    ..A
                },
            ],
            ..S
        },
        // Distinct rows, since identical rows of a table without a primary key wait for each other (finding 7103).
        ScriptTest {
            name: "a deferred unique check waits for another transaction that inserted the key",
            set_up_script: &[
                "CREATE TABLE w (a INT UNIQUE DEFERRABLE INITIALLY DEFERRED, b INT);",
                "INSERT INTO w VALUES (8, 0);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO w VALUES (9, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO w VALUES (9, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM w ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[&[T("8")], &[T("9")]],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a deferred unique check of identical rows waits for another transaction that inserted the key",
            set_up_script: &[
                "CREATE TABLE w (a INT UNIQUE DEFERRABLE INITIALLY DEFERRED);",
                "INSERT INTO w VALUES (8);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO w VALUES (9);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO w VALUES (9);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM w ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[&[T("8")], &[T("9")]],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rows that share a deferrable primary key are found through another index and changed one at a time",
            set_up_script: &[
                "CREATE TABLE d (a INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, b INT);",
                "CREATE INDEX d_b ON d (b);",
                "INSERT INTO d VALUES (1, 10), (2, 15);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d VALUES (1, 20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM d WHERE b > 0 ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("15")],
                            &[T("1"), T("20")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL enable_indexscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM d WHERE b > 0 ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("15")],
                            &[T("1"), T("20")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM d WHERE a = 1 ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("1"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET b = 21 WHERE b = 20;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM d WHERE b > 0 ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("15")],
                            &[T("1"), T("21")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM d WHERE b = 10;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM d WHERE b > 0 ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("15")],
                            &[T("1"), T("21")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM d WHERE b = 21;",
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM d ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("21")],
                            &[T("2"), T("15")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a unique constraint checked at once beside a deferred primary key",
            set_up_script: &[
                "CREATE TABLE e (a INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, u INT UNIQUE);",
                "INSERT INTO e VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO e VALUES (1, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO e VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "e_u_key""#, detail: "Key (u)=(2) already exists.", schema: "public", table: "e", constraint: "e_u_key", ..E }),
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
                    query: "INSERT INTO e VALUES (1, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM e ORDER BY u;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "e_pkey""#, detail: "Key (a)=(1) already exists.", schema: "public", table: "e", constraint: "e_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM e ORDER BY u;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
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
                    query: "INSERT INTO e VALUES (1, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM e WHERE u = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM e ORDER BY u;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "each row that shares a deferrable primary key has its own ctid",
            set_up_script: &[
                "CREATE TABLE f (a INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, b INT);",
                "INSERT INTO f VALUES (1, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO f VALUES (1, 20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(DISTINCT ctid) FROM f WHERE a = 1;",
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
                    query: "UPDATE f SET b = 21 WHERE ctid = (SELECT ctid FROM f WHERE b = 20);",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM f ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("1"), T("21")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM f WHERE ctid = (SELECT ctid FROM f WHERE b = 10);",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM f ORDER BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("21")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM f WHERE b = 21 FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("21")],
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
                    query: "SELECT a, b FROM f;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("21")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a reader of the working set fails at a key with two rows until its deferred check runs",
            set_up_script: &[
                "CREATE TABLE g (a INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, b INT);",
                "INSERT INTO g VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO g VALUES (1, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres has no working set, so the design's error is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff('HEAD', 'WORKING', 'g');",
                    expected: Expected::Error(Diagnostic { code: "55006", message: r#"cannot read the working set of table "g" because a deferred unique check is pending"#, detail: "Key (a)=(1) has more than one row until its deferred unique check runs.", hint: "Use SET CONSTRAINTS ALL IMMEDIATE to run it now.", ..E }),
                    ..A
                },
            ],
            skip: Some("dolt_diff of a working set holding two rows for one deferrable unique key succeeds, where the test expects 55006 (bug, MacDG)"),
            ..S
        },
        ScriptTest {
            name: "ANALYZE samples each row of a deferrable primary key's table once across its blocks",
            set_up_script: &[
                "CREATE TABLE big (a INT PRIMARY KEY DEFERRABLE, b INT) WITH (autovacuum_enabled = false);",
                "INSERT INTO big VALUES (1, 1);",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
                "INSERT INTO big SELECT a + (SELECT max(a) FROM big), (a + (SELECT max(a) FROM big)) % 10 FROM big;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ANALYZE big;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT reltuples FROM pg_class WHERE relname = 'big';",
                    expected: Expected::Rows {
                        columns: &[Column("reltuples", FLOAT4)],
                        rows: &[
                            &[T("16384")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT staattnum, stanullfrac, stadistinct FROM pg_statistic WHERE starelid = 'big'::regclass ORDER BY staattnum;",
                    expected: Expected::Rows {
                        columns: &[Column("staattnum", INT2), Column("stanullfrac", FLOAT4), Column("stadistinct", FLOAT4)],
                        rows: &[
                            &[T("1"), T("0"), T("-1")],
                            &[T("2"), T("0"), T("10")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relpages > 1 AS several FROM pg_class WHERE relname = 'big';",
                    expected: Expected::Rows {
                        columns: &[Column("several", BOOL)],
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
            name: "a SCROLL cursor turns back inside the rows that share a deferrable primary key",
            set_up_script: &[
                "CREATE TABLE h (a INT PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, b INT) WITH (autovacuum_enabled = false);",
                "INSERT INTO h VALUES (1, 10), (2, 20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO h VALUES (2, 21), (2, 22);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c SCROLL CURSOR FOR SELECT a, b FROM h;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 3 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("2"), T("21")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("20")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FORWARD 2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("21")],
                            &[T("2"), T("22")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("21")],
                            &[T("2"), T("20")],
                            &[T("1"), T("10")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("2"), T("21")],
                            &[T("2"), T("22")],
                        ],
                        tag: "FETCH 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 3 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("21")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE -1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("20")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("21")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("20")],
                        ],
                        tag: "FETCH 1",
                    },
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
        ScriptTest {
            name: "an update of a deferrable primary key's row that changes no key passes a FOR KEY SHARE lock",
            set_up_script: &[
                "CREATE TABLE k (a INT PRIMARY KEY DEFERRABLE, b INT);",
                "INSERT INTO k VALUES (1, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM k WHERE a = 1 FOR KEY SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE k SET b = 11 WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM k;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a delete of a deferrable primary key's row waits for a FOR KEY SHARE lock on the row before an update that changes no key",
            set_up_script: &[
                "CREATE TABLE k (a INT PRIMARY KEY DEFERRABLE, b INT);",
                "INSERT INTO k VALUES (1, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM k WHERE a = 1 FOR KEY SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE k SET b = 11 WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM k WHERE a = 1;",
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
                    query: "SELECT * FROM k;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an immediate UNIQUE index of a table without a primary key over NULLs and identical rows",
            set_up_script: &[
                "CREATE TABLE p (a INT UNIQUE);",
                "INSERT INTO p VALUES (NULL), (NULL), (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "p_a_key""#, detail: "Key (a)=(1) already exists.", schema: "public", table: "p", constraint: "p_a_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (2), (2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "p_a_key""#, detail: "Key (a)=(2) already exists.", schema: "public", table: "p", constraint: "p_a_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (1) ON CONFLICT (a) DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM p ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[&[T("1")], &[Null], &[Null]],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
