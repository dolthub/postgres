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
use harness::script::{A, BindVar, Column, Diagnostic, Dolt, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_dolt_add() {
    run_scripts(&[
        ScriptTest {
            name: "Add all using dot",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_ADD('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Add all using -A",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_ADD('-A');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Add all individually",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_ADD('t_simple','t_composite','t_array','t_serial','t_default_simple','t_checked','t_fk_parent','t_fk_child','t_unique','t_generated','t_trigger','t_default_func');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "dolt_add also stages 2 other objects", detail: "dolt_add also stages function public.f_default()\ndolt_add also stages function public.f_trigger()", ..N }],
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_ADD('function f_trigger()','function f_default()');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_ADD('sequence t_serial_pk_seq','trigger trig_trigger ON t_trigger');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
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
fn test_dolt_branch() {
    run_scripts(&[
        ScriptTest {
            name: "All branch options",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_ADD('-A');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(DOLT_COMMIT('-m', 'initial')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_BRANCH('original');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t_simple VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_ADD('-A');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(DOLT_COMMIT('-m', 'initial')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_BRANCH('-c', 'main', 'copy');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_BRANCH('-c', '-f', 'original', 'forcecopy');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('main')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Already on branch 'main'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('original')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'original'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('copy')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'copy'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('forcecopy')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'forcecopy'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_BRANCH('-d', 'forcecopy')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "Cannot delete checked out branch 'forcecopy'", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_BRANCH('-d', 'original')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_BRANCH('-m','copy','renamedcopy')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('original')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "error: tablespec 'original' did not match any table(s) known to dolt", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('copy')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "error: tablespec 'copy' did not match any table(s) known to dolt", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('renamedcopy')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'renamedcopy'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_branch_status() {
    run_scripts(&[
        ScriptTest {
            name: "Smoke test",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "SELECT DOLT_ADD('-A');",
                "SELECT length(DOLT_COMMIT('-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('original');",
                "INSERT INTO t_simple VALUES (4);",
                "SELECT DOLT_ADD('-A');",
                "SELECT length(DOLT_COMMIT('-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_BRANCH_STATUS('main', 'original');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", NUMERIC), Column("commits_behind", NUMERIC)],
                        rows: &[
                            &[T("original"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_BRANCH_STATUS('original', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("commits_ahead", NUMERIC), Column("commits_behind", NUMERIC)],
                        rows: &[
                            &[T("main"), T("1"), T("0")],
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
fn test_dolt_checkout() {
    run_scripts(&[
        ScriptTest {
            name: "All checkout options",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT DOLT_ADD('-A');",
                "SELECT length(DOLT_COMMIT('-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('original');",
                "INSERT INTO t_simple VALUES (4);",
                "INSERT INTO t_composite VALUES (3, 100);",
                "INSERT INTO t_array VALUES (ARRAY['stu']);",
                "INSERT INTO t_serial VALUES (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (98, 8);",
                "INSERT INTO t_checked VALUES (99);",
                "INSERT INTO t_fk_parent VALUES (40);",
                "INSERT INTO t_fk_child VALUES (30);",
                "INSERT INTO t_unique VALUES (10);",
                "INSERT INTO t_generated (v1) VALUES (11);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger (v1) VALUES (2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (v2) VALUES (3);",
                "SELECT DOLT_ADD('-A');",
                "SELECT length(DOLT_COMMIT('-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('-b', 'checkoutbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'checkoutbranch'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('checkoutbranch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Already on branch 'checkoutbranch'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('main')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'main'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('original')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'original'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
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
fn test_dolt_cherry_pick() {
    run_scripts(&[
        ScriptTest {
            name: "Smoke test",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT DOLT_ADD('-A');",
                "SELECT length(DOLT_COMMIT('-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('original');",
                "INSERT INTO t_simple VALUES (4);",
                "INSERT INTO t_composite VALUES (3, 100);",
                "INSERT INTO t_array VALUES (ARRAY['stu']);",
                "INSERT INTO t_serial VALUES (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (98, 8);",
                "INSERT INTO t_checked VALUES (99);",
                "INSERT INTO t_fk_parent VALUES (40);",
                "INSERT INTO t_fk_child VALUES (30);",
                "INSERT INTO t_unique VALUES (10);",
                "INSERT INTO t_generated (v1) VALUES (11);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger (v1) VALUES (2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (v2) VALUES (3);",
                "SELECT DOLT_ADD('-A');",
                "SELECT length(DOLT_COMMIT('-m', 'initial')::text) = 32;",
                "SELECT DOLT_CHECKOUT('original')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT substring(DOLT_CHERRY_PICK('main')::text, 34);",
                    expected: Expected::Rows {
                        columns: &[Column("substring", TEXT)],
                        rows: &[
                            &[T(",0,0,0)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_clean() {
    run_scripts(&[
        ScriptTest {
            name: "Clean all",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CLEAN();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
            name: "Clean all except one",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT DOLT_ADD('t_simple');",
                "SELECT length(DOLT_COMMIT('-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("15")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CLEAN();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Clean all by name",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CLEAN('t_simple','t_composite','t_array','t_serial','t_default_simple','t_checked','t_fk_parent','t_fk_child','t_unique','t_generated','t_trigger','t_default_func','f_trigger()','f_default()');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
            name: "Dry run",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CLEAN('--dry-run');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16")],
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
fn test_dolt_commit() {
    run_scripts(&[
        ScriptTest {
            name: "Stage all then commit",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_ADD('-A');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(DOLT_COMMIT('-m', 'initial')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
            name: "Commit with staging all",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
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
            name: "Commit with staging modifications",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_ADD('t_simple','t_composite','t_array');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(DOLT_COMMIT('-m', 'initial')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("13")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t_simple VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t_composite VALUES (3, 100);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t_array VALUES (ARRAY['stu']);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(DOLT_COMMIT('-a', '-m', 'initial')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("13")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Allow and skip empty",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_COMMIT('-m', 'should_error');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "nothing to commit", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_COMMIT('--skip-empty', '-m', 'should_error');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: "SELECT length(DOLT_COMMIT('--allow-empty', '-m', 'initial')::text) = 32;",
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
    ]);
}

#[test]
fn test_dolt_conflicts_resolve() {
    run_scripts(&[
        ScriptTest {
            name: "Simple conflicts resolve all using --ours",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);",
                "INSERT INTO t_simple VALUES (1, 1);",
                "SELECT DOLT_COMMIT('-Am', 'initial')",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t_simple VALUES (2, 2);",
                "SELECT DOLT_COMMIT('-Am', 'second row on main')",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t_simple VALUES (2, 3);",
                "SELECT DOLT_COMMIT('-Am', 'conflicted row on other')",
                "SELECT DOLT_CHECKOUT('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE('other', '--no-ff');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_merge", RECORD)],
                        rows: &[
                            &[T(r#"("",0,1,"conflicts found")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t_simple"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CONFLICTS_RESOLVE('--ours', '.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_COMMIT('-Am', 'commit merge');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
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
            name: "Simple conflicts resolve all using --theirs",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);",
                "INSERT INTO t_simple VALUES (1, 1);",
                "SELECT DOLT_COMMIT('-Am', 'initial')",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t_simple VALUES (2, 2);",
                "SELECT DOLT_COMMIT('-Am', 'second row on main')",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t_simple VALUES (2, 3);",
                "SELECT DOLT_COMMIT('-Am', 'conflicted row on other')",
                "SELECT DOLT_CHECKOUT('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE('other', '--no-ff');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_merge", RECORD)],
                        rows: &[
                            &[T(r#"("",0,1,"conflicts found")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t_simple"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CONFLICTS_RESOLVE('--theirs', '.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_COMMIT('-Am', 'commit merge');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
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
            name: "Simple conflicts resolve table using --ours",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);",
                "INSERT INTO t_simple VALUES (1, 1);",
                "SELECT DOLT_COMMIT('-Am', 'initial')",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t_simple VALUES (2, 2);",
                "SELECT DOLT_COMMIT('-Am', 'second row on main')",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t_simple VALUES (2, 3);",
                "SELECT DOLT_COMMIT('-Am', 'conflicted row on other')",
                "SELECT DOLT_CHECKOUT('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE('other', '--no-ff');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_merge", RECORD)],
                        rows: &[
                            &[T(r#"("",0,1,"conflicts found")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t_simple"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CONFLICTS_RESOLVE('--ours', 't_simple');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_COMMIT('-Am', 'commit merge');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
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
            name: "Simple conflicts resolve table using --theirs",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);",
                "INSERT INTO t_simple VALUES (1, 1);",
                "SELECT DOLT_COMMIT('-Am', 'initial')",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t_simple VALUES (2, 2);",
                "SELECT DOLT_COMMIT('-Am', 'second row on main')",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t_simple VALUES (2, 3);",
                "SELECT DOLT_COMMIT('-Am', 'conflicted row on other')",
                "SELECT DOLT_CHECKOUT('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE('other', '--no-ff');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_merge", RECORD)],
                        rows: &[
                            &[T(r#"("",0,1,"conflicts found")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t_simple"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CONFLICTS_RESOLVE('--theirs', 't_simple');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_COMMIT('-Am', 'commit merge');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
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
            name: "Resolve all using --ours",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, v1 INT4, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_generated (pk INT8 PRIMARY KEY, v1 INT4, v2 INT8 GENERATED ALWAYS AS (pk * 1000) STORED);",
                "CREATE TABLE t_trigger (pk INT4 PRIMARY KEY, v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (pk INT4 PRIMARY KEY, v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1, 1);",
                "INSERT INTO t_composite VALUES (1, 1, 1);",
                "INSERT INTO t_array VALUES (ARRAY['abc'], 1);",
                "INSERT INTO t_serial VALUES (DEFAULT, 1);",
                "INSERT INTO t_generated (pk, v1) VALUES (1, 1);",
                "INSERT INTO t_trigger VALUES (1, 1);",
                "INSERT INTO t_default_func (pk, v2) VALUES (1, 1);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t_simple VALUES (2, 2);",
                "INSERT INTO t_composite VALUES (2, 2, 2);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 2);",
                "INSERT INTO t_serial VALUES (DEFAULT, 2);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t_simple VALUES (2, 3);",
                "INSERT INTO t_composite VALUES (2, 2, 3);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 3);",
                "INSERT INTO t_serial VALUES (DEFAULT, 3);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 3);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 34; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 3);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 35; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE('main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Resolve all using --theirs",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, v1 INT4, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_generated (pk INT8 PRIMARY KEY, v1 INT4, v2 INT8 GENERATED ALWAYS AS (pk * 1000) STORED);",
                "CREATE TABLE t_trigger (pk INT4 PRIMARY KEY, v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (pk INT4 PRIMARY KEY, v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1, 1);",
                "INSERT INTO t_composite VALUES (1, 1, 1);",
                "INSERT INTO t_array VALUES (ARRAY['abc'], 1);",
                "INSERT INTO t_serial VALUES (DEFAULT, 1);",
                "INSERT INTO t_generated (pk, v1) VALUES (1, 1);",
                "INSERT INTO t_trigger VALUES (1, 1);",
                "INSERT INTO t_default_func (pk, v2) VALUES (1, 1);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t_simple VALUES (2, 2);",
                "INSERT INTO t_composite VALUES (2, 2, 2);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 2);",
                "INSERT INTO t_serial VALUES (DEFAULT, 2);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t_simple VALUES (2, 3);",
                "INSERT INTO t_composite VALUES (2, 2, 3);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 3);",
                "INSERT INTO t_serial VALUES (DEFAULT, 3);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 3);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 34; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 3);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 35; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE('main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Resolve individual items",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, v1 INT4, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_generated (pk INT8 PRIMARY KEY, v1 INT4, v2 INT8 GENERATED ALWAYS AS (pk * 1000) STORED);",
                "CREATE TABLE t_trigger (pk INT4 PRIMARY KEY, v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (pk INT4 PRIMARY KEY, v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1, 1);",
                "INSERT INTO t_composite VALUES (1, 1, 1);",
                "INSERT INTO t_array VALUES (ARRAY['abc'], 1);",
                "INSERT INTO t_serial VALUES (DEFAULT, 1);",
                "INSERT INTO t_generated (pk, v1) VALUES (1, 1);",
                "INSERT INTO t_trigger VALUES (1, 1);",
                "INSERT INTO t_default_func (pk, v2) VALUES (1, 1);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t_simple VALUES (2, 2);",
                "INSERT INTO t_composite VALUES (2, 2, 2);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 2);",
                "INSERT INTO t_serial VALUES (DEFAULT, 2);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t_simple VALUES (2, 3);",
                "INSERT INTO t_composite VALUES (2, 2, 3);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 3);",
                "INSERT INTO t_serial VALUES (DEFAULT, 3);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 3);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 34; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 3);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 35; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE('main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_diff() {
    run_scripts(&[
        ScriptTest {
            name: "Single commit",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('original');",
                "INSERT INTO t_simple VALUES (4);",
                "INSERT INTO t_composite VALUES (3, 100);",
                "INSERT INTO t_array VALUES (ARRAY['stu']);",
                "INSERT INTO t_serial VALUES (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (98, 8);",
                "INSERT INTO t_checked VALUES (99);",
                "INSERT INTO t_fk_parent VALUES (40);",
                "INSERT INTO t_fk_child VALUES (30);",
                "INSERT INTO t_unique VALUES (10);",
                "INSERT INTO t_generated (v1) VALUES (11);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger (v1) VALUES (2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (v2) VALUES (3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_pk FROM DOLT_DIFF('main', 'original', 't_simple');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_pk1, from_pk2 FROM DOLT_DIFF('main', 'original', 't_composite');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk1", INT2), Column("from_pk2", INT8)],
                        rows: &[
                            &[T("3"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_pk FROM DOLT_DIFF('main', 'original', 't_array');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", TEXT_ARRAY)],
                        rows: &[
                            &[T("{stu}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_pk FROM DOLT_DIFF('main', 'original', 't_serial');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_v1, from_v2 FROM DOLT_DIFF('main', 'original', 't_default_simple');",
                    expected: Expected::Rows {
                        columns: &[Column("from_v1", INT8), Column("from_v2", INT8)],
                        rows: &[
                            &[T("98"), T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_v1 FROM DOLT_DIFF('main', 'original', 't_checked');",
                    expected: Expected::Rows {
                        columns: &[Column("from_v1", NUMERIC)],
                        rows: &[
                            &[T("99")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_pk FROM DOLT_DIFF('main', 'original', 't_fk_parent');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4)],
                        rows: &[
                            &[T("40")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_pk FROM DOLT_DIFF('main', 'original', 't_fk_child');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_v1 FROM DOLT_DIFF('main', 'original', 't_unique');",
                    expected: Expected::Rows {
                        columns: &[Column("from_v1", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_v1, from_v2 FROM DOLT_DIFF('main', 'original', 't_generated');",
                    expected: Expected::Rows {
                        columns: &[Column("from_v1", INT8), Column("from_v2", INT8)],
                        rows: &[
                            &[T("11"), T("11000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_v1 FROM DOLT_DIFF('main', 'original', 't_trigger');",
                    expected: Expected::Rows {
                        columns: &[Column("from_v1", INT8)],
                        rows: &[
                            &[T("66")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_v1, from_v2 FROM DOLT_DIFF('main', 'original', 't_default_func');",
                    expected: Expected::Rows {
                        columns: &[Column("from_v1", INT8), Column("from_v2", INT8)],
                        rows: &[
                            &[T("34"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' wording for the error.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_DIFF('main', 'original', 'f_trigger()');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"f_trigger()\" does not exist", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' wording for the error.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_DIFF('main', 'original', 'f_default()');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"f_default()\" does not exist", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' wording for the error.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_DIFF('main', 'original', 't_trigger.trig_trigger');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t_trigger.trig_trigger\" does not exist", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' wording for the error.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_DIFF('main', 'original', 't_serial_pk_seq');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"t_serial_pk_seq\" does not exist", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt table functions WITH ORDINALITY",
            set_up_script: &[
                "CREATE TABLE t1 (pk INT4 PRIMARY KEY, v1 TEXT);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "INSERT INTO t1 VALUES (1, 'one'), (2, 'two');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_pk, to_pk, ordinality FROM dolt_diff('HEAD', 'WORKING', 't1') WITH ORDINALITY;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4), Column("ordinality", INT8)],
                        rows: &[
                            &[T("added"), Null, T("1"), T("1")],
                            &[T("added"), Null, T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT ord, to_pk1 FROM dolt_diff('HEAD', 'WORKING', 't1') WITH ORDINALITY AS a(to_pk1, to_v11, to_commit1, to_commit_date1, from_pk1, from_v11, from_commit1, from_commit_date1, diff_type1, ord);",
                    expected: Expected::Rows {
                        columns: &[Column("ord", INT8), Column("to_pk1", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT message, ordinality FROM dolt_log() WITH ORDINALITY LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("ordinality", INT8)],
                        rows: &[
                            &[T("initial"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, ordinality FROM dolt_diff_stat('HEAD', 'WORKING') WITH ORDINALITY;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("ordinality", INT8)],
                        rows: &[
                            &[T("public.t1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT to_table_name, diff_type, ordinality FROM dolt_diff_summary('HEAD', 'WORKING') WITH ORDINALITY;",
                    expected: Expected::Rows {
                        columns: &[Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("ordinality", INT8)],
                        rows: &[
                            &[T("public.t1"), T("modified"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, diff_type, ordinality FROM dolt_patch('HEAD', 'WORKING') WITH ORDINALITY;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("diff_type", TEXT), Column("ordinality", INT8)],
                        rows: &[
                            &[T("public.t1"), T("data"), T("1")],
                            &[T("public.t1"), T("data"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'WORKING') WITH ORDINALITY;",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT), Column("ordinality", INT8)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "IN expressions over table function commit columns",
            set_up_script: &[
                "CREATE TABLE test (id INT PRIMARY KEY, val INT)",
                "INSERT INTO test VALUES (1, 1)",
                "SELECT dolt_commit('-Am', 'commit 1')",
                "INSERT INTO test VALUES (2, 2)",
                "SELECT dolt_commit('-Am', 'commit 2')",
                "UPDATE test SET val = 3 WHERE id = 1",
                "SELECT dolt_commit('-Am', 'commit 3')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_to_commit, dolt_from_commit, to_id FROM dolt_diff('HEAD~2', 'HEAD', 'test') WHERE dolt_to_commit IN ('HEAD', 'WORKING') ORDER BY to_id",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_to_commit", TEXT), Column("dolt_from_commit", TEXT), Column("to_id", INT4)],
                        rows: &[
                            &[T("HEAD"), T("HEAD~2"), T("1")],
                            &[T("HEAD"), T("HEAD~2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT to_id FROM dolt_diff('HEAD~2', 'HEAD', 'test') WHERE dolt_to_commit IN (HASHOF('HEAD'), HASHOF('HEAD~1'))",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT to_id FROM dolt_diff('HEAD~2', 'HEAD', 'test') WHERE dolt_to_commit NOT IN ('HEAD') ORDER BY to_id",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('HEAD') WHERE commit_hash IN (HASHOF('HEAD'), HASHOF('HEAD~1'))",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log('HEAD') WHERE commit_hash IN ('not_a_hash')",
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
fn test_dolt_diff_stat() {
    run_scripts(&[
        ScriptTest {
            name: "Single commit",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('original');",
                "INSERT INTO t_simple VALUES (4);",
                "INSERT INTO t_composite VALUES (3, 100);",
                "INSERT INTO t_array VALUES (ARRAY['stu']);",
                "INSERT INTO t_serial VALUES (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (98, 8);",
                "INSERT INTO t_checked VALUES (99);",
                "INSERT INTO t_fk_parent VALUES (40);",
                "INSERT INTO t_fk_child VALUES (30);",
                "INSERT INTO t_unique VALUES (10);",
                "INSERT INTO t_generated (v1) VALUES (11);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger (v1) VALUES (2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (v2) VALUES (3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_STAT('main', 'original');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_simple');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t_simple")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_composite');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t_composite")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_array');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t_array")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_serial');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t_serial")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_default_simple');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t_default_simple")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_checked');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t_checked")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_fk_parent');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t_fk_parent")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_fk_child');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t_fk_child")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_unique');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t_unique")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_generated');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t_generated")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_trigger');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t_trigger")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_default_func');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t_default_func")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 'function public.f_trigger()');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 'function public.f_default()');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM DOLT_DIFF_STAT('main', 'original', 't_serial_pk_seq');",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_diff_summary() {
    run_scripts(&[
        ScriptTest {
            name: "Single commit",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('original');",
                "INSERT INTO t_simple VALUES (4);",
                "INSERT INTO t_composite VALUES (3, 100);",
                "INSERT INTO t_array VALUES (ARRAY['stu']);",
                "INSERT INTO t_serial VALUES (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (98, 8);",
                "INSERT INTO t_checked VALUES (99);",
                "INSERT INTO t_fk_parent VALUES (40);",
                "INSERT INTO t_fk_child VALUES (30);",
                "INSERT INTO t_unique VALUES (10);",
                "INSERT INTO t_generated (v1) VALUES (11);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger (v1) VALUES (2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (v2) VALUES (3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_DIFF_SUMMARY('main', 'original');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_simple');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.t_simple")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_composite');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.t_composite")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_array');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.t_array")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_serial');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.t_serial")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_default_simple');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.t_default_simple")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_checked');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.t_checked")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_fk_parent');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.t_fk_parent")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_fk_child');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.t_fk_child")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_unique');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.t_unique")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_generated');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.t_generated")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_trigger');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.t_trigger")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_default_func');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("public.t_default_func")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 'function public.f_trigger()');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("function public.f_trigger()")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 'function public.f_default()');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                            &[T("function public.f_default()")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT from_table_name FROM DOLT_DIFF_SUMMARY('main', 'original', 't_serial_pk_seq');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_function_smoke_tests() {
    run_scripts(&[
        ScriptTest {
            name: "smoke test select dolt_add and dolt_commit",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_add('.')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_commit('-am', 'new table')",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select count(*) from dolt.log",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select message from dolt.log order by date desc limit 1",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "smoke test select dolt_merge",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key);",
                "SELECT DOLT_COMMIT('-Am', 'new table');",
                "SELECT DOLT_CHECKOUT('-b', 'new-branch');",
                "CREATE TABLE t2 (pk int primary key);",
                "SELECT DOLT_COMMIT('-Am', 'new table on new branch');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE_BASE('main', 'new-branch');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'main'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select count(*) from dolt.log",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE('new-branch', '--no-ff', '-m', 'merge new-branch into main');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select count(*) from dolt.log",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
            name: "smoke test select dolt_merge dirty working set, same table",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key);",
                "SELECT DOLT_COMMIT('-Am', 'new table');",
                "INSERT INTO t1 VALUES (1);",
                "SELECT DOLT_CHECKOUT('-b', 'new-branch');",
                "INSERT INTO t1 VALUES (2);",
                "SELECT DOLT_COMMIT('-Am', 'new row on new branch');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE_BASE('main', 'new-branch');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'main'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE('new-branch', '--no-ff', '-m', 'merge new-branch into main');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: r#"error: local changes would be stomped by merge:
	t1
 Please commit your changes before you merge."#, ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "smoke test select dolt_merge dirty working set, different tables",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key);",
                "SELECT DOLT_COMMIT('-Am', 'new table');",
                "INSERT INTO t1 VALUES (1);",
                "SELECT DOLT_CHECKOUT('-b', 'new-branch');",
                "CREATE TABLE t2 (pk int primary key);",
                "SELECT DOLT_COMMIT('-Am', 'new row on new branch');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE_BASE('main', 'new-branch');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'main'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE('new-branch', '--no-ff', '-m', 'merge new-branch into main');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "smoke test select dolt_reset",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key);",
                "INSERT INTO t1 VALUES (1);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_ADD('t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_RESET('t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "smoke test select dolt_clean",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key);",
                "INSERT INTO t1 VALUES (1);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CLEAN('t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (pk int primary key);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CLEAN();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                // Doltgrust-specific: an earlier Dolt statement changed state Postgres lacks, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "CREATE TABLE t1 (pk int primary key);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CLEAN();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clean", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "smoke test select dolt_checkout(table)",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key);",
                "INSERT INTO t1 VALUES (1);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "smoke test select dolt diff functions and tables",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key);",
                "INSERT INTO t1 VALUES (1);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING')",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t1"), T("0"), T("1"), T("0"), T("0"), T("1"), T("0"), T("0"), T("0"), T("1"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t1"), T("0"), T("1"), T("0"), T("0"), T("1"), T("0"), T("0"), T("0"), T("1"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING')",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t1"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t1"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_pk, to_pk FROM dolt_diff('HEAD', 'WORKING', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("added"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_pk, to_pk FROM dolt_diff('HEAD', 'WORKING', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("added"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_pk, to_pk FROM dolt_commit_diff_t1 WHERE dolt_to_commit=HASHOF('main') AND dolt_from_commit='WORKING'",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.diff",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("committer", TEXT), Column("email", TEXT), Column("date", TIMESTAMPTZ), Column("message", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL), Column("author", TEXT), Column("author_email", TEXT), Column("author_date", TIMESTAMPTZ)],
                        rows: &[
                            &[T("WORKING"), T("public.t1"), Null, Null, Null, Null, T("t"), T("t"), Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD', 'WORKING')",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", NUMERIC), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t1"), T("schema"), T(r#"CREATE TABLE "t1" (
  "pk" integer NOT NULL,
  PRIMARY KEY ("pk")
);"#)],
                            &[T("2"), T("public.t1"), T("data"), T(r#"INSERT INTO "t1" ("pk") VALUES (1);"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD', 'WORKING', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", NUMERIC), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t1"), T("schema"), T(r#"CREATE TABLE "t1" (
  "pk" integer NOT NULL,
  PRIMARY KEY ("pk")
);"#)],
                            &[T("2"), T("public.t1"), T("data"), T(r#"INSERT INTO "t1" ("pk") VALUES (1);"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'WORKING')",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T(""), T("public.t1"), T(""), T(r#"CREATE TABLE "t1" (
  "pk" integer NOT NULL,
  PRIMARY KEY ("pk")
);"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'WORKING', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T(""), T("public.t1"), T(""), T(r#"CREATE TABLE "t1" (
  "pk" integer NOT NULL,
  PRIMARY KEY ("pk")
);"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: the Go server's output was wrong here, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('select * from t1 as of main', 'select * from t1')",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "main""#, position: 24, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "smoke test select dolt diff functions and tables for multiple schemas",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key);",
                "INSERT INTO t1 VALUES (1);",
                "CREATE SCHEMA testschema;",
                "CREATE TABLE testschema.t2 (pk int primary key);",
                "INSERT INTO testschema.t2 VALUES (1);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t1"), T("f"), T("new table")],
                            &[T("testschema.t2"), T("f"), T("new table")],
                            &[T("testschema"), T("f"), T("new schema")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING')",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t1"), T("0"), T("1"), T("0"), T("0"), T("1"), T("0"), T("0"), T("0"), T("1"), T("0"), T("1")],
                            &[T("testschema.t2"), T("0"), T("1"), T("0"), T("0"), T("1"), T("0"), T("0"), T("0"), T("1"), T("0"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("public.t1"), T("0"), T("1"), T("0"), T("0"), T("1"), T("0"), T("0"), T("0"), T("1"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_stat('HEAD', 'WORKING', 't2')",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_unmodified", INT8), Column("rows_added", INT8), Column("rows_deleted", INT8), Column("rows_modified", INT8), Column("cells_added", INT8), Column("cells_deleted", INT8), Column("cells_modified", INT8), Column("old_row_count", INT8), Column("new_row_count", INT8), Column("old_cell_count", INT8), Column("new_cell_count", INT8)],
                        rows: &[
                            &[T("testschema.t2"), T("0"), T("1"), T("0"), T("0"), T("1"), T("0"), T("0"), T("0"), T("1"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING')",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t1"), T("added"), T("t"), T("t")],
                            &[T(""), T("testschema.t2"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("public.t1"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD', 'WORKING', 't2')",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T(""), T("testschema.t2"), T("added"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_pk, to_pk FROM dolt_diff('HEAD', 'WORKING', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("added"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_pk, to_pk FROM dolt_diff('HEAD', 'WORKING', 't2')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("added"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_pk, to_pk FROM dolt_commit_diff_t1 WHERE dolt_to_commit=HASHOF('main') AND dolt_from_commit='WORKING'",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt.diff",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT), Column("committer", TEXT), Column("email", TEXT), Column("date", TIMESTAMPTZ), Column("message", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL), Column("author", TEXT), Column("author_email", TEXT), Column("author_date", TIMESTAMPTZ)],
                        rows: &[
                            &[T("WORKING"), T("public.t1"), Null, Null, Null, Null, T("t"), T("t"), Null, Null, Null],
                            &[T("WORKING"), T("testschema.t2"), Null, Null, Null, Null, T("t"), T("t"), Null, Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD', 'WORKING')",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", NUMERIC), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t1"), T("schema"), T(r#"CREATE TABLE "t1" (
  "pk" integer NOT NULL,
  PRIMARY KEY ("pk")
);"#)],
                            &[T("2"), T("public.t1"), T("data"), T(r#"INSERT INTO "t1" ("pk") VALUES (1);"#)],
                            &[T("3"), T("testschema.t2"), T("schema"), T(r#"CREATE TABLE "t2" (
  "pk" integer NOT NULL,
  PRIMARY KEY ("pk")
);"#)],
                            &[T("4"), T("testschema.t2"), T("data"), T(r#"INSERT INTO "t2" ("pk") VALUES (1);"#)],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD', 'WORKING', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", NUMERIC), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.t1"), T("schema"), T(r#"CREATE TABLE "t1" (
  "pk" integer NOT NULL,
  PRIMARY KEY ("pk")
);"#)],
                            &[T("2"), T("public.t1"), T("data"), T(r#"INSERT INTO "t1" ("pk") VALUES (1);"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD', 'WORKING', 't2')",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", NUMERIC), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("testschema.t2"), T("schema"), T(r#"CREATE TABLE "t2" (
  "pk" integer NOT NULL,
  PRIMARY KEY ("pk")
);"#)],
                            &[T("2"), T("testschema.t2"), T("data"), T(r#"INSERT INTO "t2" ("pk") VALUES (1);"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'WORKING')",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T(""), T("public.t1"), T(""), T(r#"CREATE TABLE "t1" (
  "pk" integer NOT NULL,
  PRIMARY KEY ("pk")
);"#)],
                            &[T(""), T("testschema.t2"), T(""), T(r#"CREATE TABLE "t2" (
  "pk" integer NOT NULL,
  PRIMARY KEY ("pk")
);"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'WORKING', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T(""), T("public.t1"), T(""), T(r#"CREATE TABLE "t1" (
  "pk" integer NOT NULL,
  PRIMARY KEY ("pk")
);"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_schema_diff('HEAD', 'WORKING', 't2')",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T(""), T("testschema.t2"), T(""), T(r#"CREATE TABLE "t2" (
  "pk" integer NOT NULL,
  PRIMARY KEY ("pk")
);"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: the Go server's output was wrong here, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('select * from t1 as of main', 'select * from t1')",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "main""#, position: 24, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                // Doltgrust-specific: the Go server's output was wrong here, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_query_diff('select * from t2 as of main', 'select * from t2')",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "main""#, position: 24, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_patch works with JSONB columns",
            set_up_script: &[
                "CREATE TABLE repro (pk int primary key, data jsonb);",
                r#"INSERT INTO repro VALUES (1, '{"text": "hello"}');"#,
            ],
            assertions: &[
                // Doltgrust-specific: Go's output, with the string literal quoted as Postgres reads it instead of with MySQL's backslash escapes.
                ScriptTestAssertion {
                    query: "SELECT statement_order, table_name, diff_type, statement FROM dolt_patch('HEAD', 'WORKING', 'repro')",
                    expected: Expected::Rows {
                        columns: &[Column("statement_order", NUMERIC), Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("1"), T("public.repro"), T("schema"), T(r#"CREATE TABLE "repro" (
  "pk" integer NOT NULL,
  "data" jsonb,
  PRIMARY KEY ("pk")
);"#)],
                            &[T("2"), T("public.repro"), T("data"), T(r#"INSERT INTO "repro" ("pk","data") VALUES (1,'{"text": "hello"}');"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_diff_type, from_pk, to_pk FROM dolt_diff('HEAD', 'WORKING', 'repro')",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_diff_type", TEXT), Column("from_pk", INT4), Column("to_pk", INT4)],
                        rows: &[
                            &[T("added"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_PREVIEW_MERGE_CONFLICTS basic functionality",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key, c1 int);",
                "INSERT INTO t1 VALUES (1, 10), (2, 20);",
                "SELECT DOLT_COMMIT('-Am', 'initial commit');",
                "SELECT DOLT_CHECKOUT('-b', 'branch1');",
                "UPDATE t1 SET c1 = 100 WHERE pk = 1;",
                "SELECT DOLT_COMMIT('-am', 'update on branch1');",
                "SELECT DOLT_CHECKOUT('main');",
                "UPDATE t1 SET c1 = 200 WHERE pk = 1;",
                "SELECT DOLT_COMMIT('-am', 'update on main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('main', 'branch1')",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", NUMERIC), Column("num_schema_conflicts", NUMERIC)],
                        rows: &[
                            &[T("public.t1"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_c1, our_pk, our_c1, dolt_our_diff_type, their_pk, their_c1, dolt_their_diff_type FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", INT4), Column("our_pk", INT4), Column("our_c1", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("10"), T("1"), T("200"), T("modified"), T("1"), T("100"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_PREVIEW_MERGE_CONFLICTS with no conflicts",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key, c1 int);",
                "INSERT INTO t1 VALUES (1, 10), (2, 20);",
                "SELECT DOLT_COMMIT('-Am', 'initial commit');",
                "SELECT DOLT_CHECKOUT('-b', 'branch1');",
                "INSERT INTO t1 VALUES (3, 30);",
                "SELECT DOLT_COMMIT('-am', 'insert on branch1');",
                "SELECT DOLT_CHECKOUT('main');",
                "INSERT INTO t1 VALUES (4, 40);",
                "SELECT DOLT_COMMIT('-am', 'insert on main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('main', 'branch1')",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", NUMERIC), Column("num_schema_conflicts", NUMERIC)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_c1, our_pk, our_c1, dolt_our_diff_type, their_pk, their_c1, dolt_their_diff_type FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", INT4), Column("our_pk", INT4), Column("our_c1", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_PREVIEW_MERGE_CONFLICTS with multiple tables",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key, c1 int);",
                "CREATE TABLE t2 (pk int primary key, c1 varchar(20));",
                "INSERT INTO t1 VALUES (1, 10);",
                "INSERT INTO t2 VALUES (1, 'initial');",
                "SELECT DOLT_COMMIT('-Am', 'initial commit');",
                "SELECT DOLT_CHECKOUT('-b', 'branch1');",
                "UPDATE t1 SET c1 = 100 WHERE pk = 1;",
                "UPDATE t2 SET c1 = 'branch1' WHERE pk = 1;",
                "SELECT DOLT_COMMIT('-am', 'updates on branch1');",
                "SELECT DOLT_CHECKOUT('main');",
                "UPDATE t1 SET c1 = 200 WHERE pk = 1;",
                "UPDATE t2 SET c1 = 'main' WHERE pk = 1;",
                "SELECT DOLT_COMMIT('-am', 'updates on main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('main', 'branch1') ORDER BY 'table'",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", NUMERIC), Column("num_schema_conflicts", NUMERIC)],
                        rows: &[
                            &[T("public.t1"), T("1"), T("0")],
                            &[T("public.t2"), T("1"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 't2')",
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
            name: "DOLT_PREVIEW_MERGE_CONFLICTS with schema conflicts",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key, c1 int);",
                "INSERT INTO t1 VALUES (1, 10);",
                "SELECT DOLT_COMMIT('-Am', 'initial commit');",
                "SELECT DOLT_CHECKOUT('-b', 'branch1');",
                "ALTER TABLE t1 ADD COLUMN c2 varchar(50);",
                "SELECT DOLT_COMMIT('-am', 'add column on branch1');",
                "SELECT DOLT_CHECKOUT('main');",
                "ALTER TABLE t1 ADD COLUMN c2 int;",
                "SELECT DOLT_COMMIT('-am', 'add same column different type on main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('main', 'branch1')",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", NUMERIC), Column("num_schema_conflicts", NUMERIC)],
                        rows: &[
                            &[T("public.t1"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 't1')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "schema conflicts found: 1", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_PREVIEW_MERGE_CONFLICTS with multiple schemas",
            set_up_script: &[
                "CREATE SCHEMA test_schema;",
                "CREATE TABLE t1 (pk int primary key, c1 int);",
                "CREATE TABLE test_schema.t2 (pk int primary key, c1 int);",
                "INSERT INTO t1 VALUES (1, 10);",
                "INSERT INTO test_schema.t2 VALUES (1, 20);",
                "SELECT DOLT_COMMIT('-Am', 'initial commit');",
                "SELECT DOLT_CHECKOUT('-b', 'branch1');",
                "UPDATE t1 SET c1 = 100 WHERE pk = 1;",
                "UPDATE test_schema.t2 SET c1 = 200 WHERE pk = 1;",
                "SELECT DOLT_COMMIT('-am', 'updates on branch1');",
                "SELECT DOLT_CHECKOUT('main');",
                "UPDATE t1 SET c1 = 300 WHERE pk = 1;",
                "UPDATE test_schema.t2 SET c1 = 400 WHERE pk = 1;",
                "SELECT DOLT_COMMIT('-am', 'updates on main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('main', 'branch1') ORDER BY 'table'",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", NUMERIC), Column("num_schema_conflicts", NUMERIC)],
                        rows: &[
                            &[T("public.t1"), T("1"), T("0")],
                            &[T("test_schema.t2"), T("1"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 't2')",
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
            name: "DOLT_PREVIEW_MERGE_CONFLICTS with multiple schemas, same name",
            set_up_script: &[
                "CREATE SCHEMA test_schema;",
                "CREATE TABLE t1 (pk int primary key, c1 int);",
                "CREATE TABLE test_schema.t1 (pk int primary key, c2 int);",
                "INSERT INTO t1 VALUES (1, 10);",
                "INSERT INTO test_schema.t1 VALUES (1, 20);",
                "SELECT DOLT_COMMIT('-Am', 'initial commit');",
                "SELECT DOLT_CHECKOUT('-b', 'branch1');",
                "UPDATE t1 SET c1 = 100 WHERE pk = 1;",
                "UPDATE test_schema.t1 SET c2 = 200 WHERE pk = 1;",
                "SELECT DOLT_COMMIT('-am', 'updates on branch1');",
                "SELECT DOLT_CHECKOUT('main');",
                "UPDATE t1 SET c1 = 300 WHERE pk = 1;",
                "UPDATE test_schema.t1 SET c2 = 400 WHERE pk = 1;",
                "SELECT DOLT_COMMIT('-am', 'updates on main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('main', 'branch1') ORDER BY 'table'",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", NUMERIC), Column("num_schema_conflicts", NUMERIC)],
                        rows: &[
                            &[T("public.t1"), T("1"), T("0")],
                            &[T("test_schema.t1"), T("1"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT base_c1 FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("base_c1", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: the Go server's output was wrong here, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT base_c2 FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 't1')",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "base_c2" does not exist"#, position: 8, ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SET search_path TO test_schema;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT base_c2 FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 't1')",
                    expected: Expected::Rows {
                        columns: &[Column("base_c2", INT4)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: the Go server's output was wrong here, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT base_c1 FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 't1')",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "base_c1" does not exist"#, position: 8, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DOLT_PREVIEW_MERGE_CONFLICTS error cases",
            set_up_script: &[
                "CREATE TABLE t1 (pk int primary key, c1 int);",
                "SELECT DOLT_COMMIT('-Am', 'initial commit');",
                "SELECT DOLT_CHECKOUT('-b', 'branch1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('nonexistent-branch', 'main')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "branch not found: nonexistent-branch", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('main', 'branch1', 'table')",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function 'dolt_preview_merge_conflicts_summary' expected 2 arguments, 3 received", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('main', 'nonexistent-branch')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "branch not found: nonexistent-branch", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('', 'main')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "left branch name cannot be empty", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('main', '')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "right branch name cannot be empty", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY(NULL, 'main')",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_preview_merge_conflicts_summary: NULL", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('main', NULL)",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_preview_merge_conflicts_summary: NULL", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS('nonexistent-branch', 'main', 't1')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "branch not found: nonexistent-branch", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'nonexistent-branch', 't1')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "branch not found: nonexistent-branch", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1')",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function 'dolt_preview_merge_conflicts' expected 3 arguments, 2 received", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 't1', 'extra')",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function 'dolt_preview_merge_conflicts' expected 3 arguments, 4 received", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS('', 'main', 't1')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "string is not a valid branch or hash", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', '', 't1')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "string is not a valid branch or hash", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS(NULL, 'main', 't1')",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_preview_merge_conflicts: NULL", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', NULL, 't1')",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_preview_merge_conflicts: NULL", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', NULL)",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_preview_merge_conflicts: NULL", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', 'nonexistent_table')",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "table not found: public.nonexistent_table", ..E }),
                    skip: Some("Go reports the missing table in MySQL's words; the Rust server reports the missing relation as Postgres does"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'branch1', '')",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "table name cannot be empty", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_gc() {
    run_scripts(&[
        ScriptTest {
            name: "Full",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('original');",
                "INSERT INTO t_simple VALUES (4);",
                "INSERT INTO t_composite VALUES (3, 100);",
                "INSERT INTO t_array VALUES (ARRAY['stu']);",
                "INSERT INTO t_serial VALUES (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (98, 8);",
                "INSERT INTO t_checked VALUES (99);",
                "INSERT INTO t_fk_parent VALUES (40);",
                "INSERT INTO t_fk_child VALUES (30);",
                "INSERT INTO t_unique VALUES (10);",
                "INSERT INTO t_generated (v1) VALUES (11);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger (v1) VALUES (2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (v2) VALUES (3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_GC();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
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
            name: "Shallow",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('original');",
                "INSERT INTO t_simple VALUES (4);",
                "INSERT INTO t_composite VALUES (3, 100);",
                "INSERT INTO t_array VALUES (ARRAY['stu']);",
                "INSERT INTO t_serial VALUES (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (98, 8);",
                "INSERT INTO t_checked VALUES (99);",
                "INSERT INTO t_fk_parent VALUES (40);",
                "INSERT INTO t_fk_child VALUES (30);",
                "INSERT INTO t_unique VALUES (10);",
                "INSERT INTO t_generated (v1) VALUES (11);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger (v1) VALUES (2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (v2) VALUES (3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_GC('--shallow');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
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
fn test_dolt_log() {
    run_scripts(&[
        ScriptTest {
            name: "Smoke test",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('original');",
                "INSERT INTO t_simple VALUES (4);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_LOG('main');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_LOG('original');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
fn test_dolt_merge() {
    run_scripts(&[
        ScriptTest {
            name: "Merge without conflicts",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, v1 INT4, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_generated (pk INT8 PRIMARY KEY, v1 INT4, v2 INT8 GENERATED ALWAYS AS (pk * 1000) STORED);",
                "CREATE TABLE t_trigger (pk INT4 PRIMARY KEY, v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (pk INT4 PRIMARY KEY, v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1, 1);",
                "INSERT INTO t_composite VALUES (1, 1, 1);",
                "INSERT INTO t_array VALUES (ARRAY['abc'], 1);",
                "INSERT INTO t_serial VALUES (DEFAULT, 1);",
                "INSERT INTO t_generated (pk, v1) VALUES (1, 1);",
                "INSERT INTO t_trigger VALUES (1, 1);",
                "INSERT INTO t_default_func (pk, v2) VALUES (1, 1);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t_simple VALUES (2, 2);",
                "INSERT INTO t_composite VALUES (2, 2, 2);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 2);",
                "INSERT INTO t_serial VALUES (DEFAULT, 2);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t_simple VALUES (3, 3);",
                "INSERT INTO t_composite VALUES (3, 2, 3);",
                "INSERT INTO t_array VALUES (ARRAY['dfe'], 3);",
                "INSERT INTO t_serial VALUES (4, 3);",
                "INSERT INTO t_generated (pk, v1) VALUES (3, 3);",
                "INSERT INTO t_trigger VALUES (3, 3);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT4 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (3, 3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: r#"SELECT strpos(DOLT_MERGE('main')::text, '0,0,"merge successful"') > 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("strpos > 1", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("The setup changes f_default's return type with CREATE OR REPLACE FUNCTION, which Go allows and Postgres refuses with 42P13"),
            ..S
        },
        ScriptTest {
            name: "Merge with conflicts",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, v1 INT4, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_generated (pk INT8 PRIMARY KEY, v1 INT4, v2 INT8 GENERATED ALWAYS AS (pk * 1000) STORED);",
                "CREATE TABLE t_trigger (pk INT4 PRIMARY KEY, v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (pk INT4 PRIMARY KEY, v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1, 1);",
                "INSERT INTO t_composite VALUES (1, 1, 1);",
                "INSERT INTO t_array VALUES (ARRAY['abc'], 1);",
                "INSERT INTO t_serial VALUES (DEFAULT, 1);",
                "INSERT INTO t_generated (pk, v1) VALUES (1, 1);",
                "INSERT INTO t_trigger VALUES (1, 1);",
                "INSERT INTO t_default_func (pk, v2) VALUES (1, 1);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t_simple VALUES (2, 2);",
                "INSERT INTO t_composite VALUES (2, 2, 2);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 2);",
                "INSERT INTO t_serial VALUES (DEFAULT, 2);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t_simple VALUES (2, 3);",
                "INSERT INTO t_composite VALUES (2, 2, 3);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 3);",
                "INSERT INTO t_serial VALUES (DEFAULT, 3);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 3);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 34; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 3);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 35; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE('main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Fast forward",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('original');",
                "INSERT INTO t_simple VALUES (4);",
                "INSERT INTO t_composite VALUES (3, 100);",
                "INSERT INTO t_array VALUES (ARRAY['stu']);",
                "INSERT INTO t_serial VALUES (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (98, 8);",
                "INSERT INTO t_checked VALUES (99);",
                "INSERT INTO t_fk_parent VALUES (40);",
                "INSERT INTO t_fk_child VALUES (30);",
                "INSERT INTO t_unique VALUES (10);",
                "INSERT INTO t_generated (v1) VALUES (11);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger (v1) VALUES (2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (v2) VALUES (3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_CHECKOUT('original');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: r#"SELECT strpos(DOLT_MERGE('main')::text, '1,0,"merge successful"') > 1;"#,
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
            name: "--no-ff",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('original');",
                "INSERT INTO t_simple VALUES (4);",
                "INSERT INTO t_composite VALUES (3, 100);",
                "INSERT INTO t_array VALUES (ARRAY['stu']);",
                "INSERT INTO t_serial VALUES (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (98, 8);",
                "INSERT INTO t_checked VALUES (99);",
                "INSERT INTO t_fk_parent VALUES (40);",
                "INSERT INTO t_fk_child VALUES (30);",
                "INSERT INTO t_unique VALUES (10);",
                "INSERT INTO t_generated (v1) VALUES (11);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger (v1) VALUES (2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (v2) VALUES (3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_CHECKOUT('original');",
            ],
            assertions: &[
                // Doltgrust-specific: Go's output, with the column named ?column? as Postgres names it rather than after go-mysql-server's expression text.
                ScriptTestAssertion {
                    query: "SELECT strpos(DOLT_MERGE('main', '--no-ff', '-m', 'merge_commit')::text, 'merge successful') > 1;",
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
            name: "Merge with nested parentheses in default, generated, and check expressions",
            set_up_script: &[
                "CREATE TABLE t3324 (pk INT4 PRIMARY KEY, a INT4 DEFAULT (1 + 1) * 2, b INT4 GENERATED ALWAYS AS ((pk + 1) * 2) STORED, CONSTRAINT c3324 CHECK (NOT (pk = 0 OR pk + 1 = 0) AND ((pk + 1) * 2) > 3));",
                "INSERT INTO t3324 (pk) VALUES (1);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t3324 (pk) VALUES (2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'main')::text) = 32;",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t3324 (pk) VALUES (3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'other')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Go's output, with the column named ?column? as Postgres names it rather than after go-mysql-server's expression text.
                ScriptTestAssertion {
                    query: "SELECT strpos(DOLT_MERGE('main', '--no-ff', '-m', 'merge_commit')::text, 'merge successful') > 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t3324 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("4"), T("4")],
                            &[T("2"), T("4"), T("6")],
                            &[T("3"), T("4"), T("8")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO t3324 (pk) VALUES (0);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"Check constraint "c3324" violated"#, ..E }),
                    skip: Some("Go reports the violated check constraint in go-mysql-server's words; the Rust server reports it as Postgres does"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_preview_merge_conflicts() {
    run_scripts(&[
        ScriptTest {
            name: "Preview conflicts",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, v1 INT4, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_generated (pk INT8 PRIMARY KEY, v1 INT4, v2 INT8 GENERATED ALWAYS AS (pk * 1000) STORED);",
                "CREATE TABLE t_trigger (pk INT4 PRIMARY KEY, v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (pk INT4 PRIMARY KEY, v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1, 1);",
                "INSERT INTO t_composite VALUES (1, 1, 1);",
                "INSERT INTO t_array VALUES (ARRAY['abc'], 1);",
                "INSERT INTO t_serial VALUES (DEFAULT, 1);",
                "INSERT INTO t_generated (pk, v1) VALUES (1, 1);",
                "INSERT INTO t_trigger VALUES (1, 1);",
                "INSERT INTO t_default_func (pk, v2) VALUES (1, 1);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t_simple VALUES (2, 2);",
                "INSERT INTO t_composite VALUES (2, 2, 2);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 2);",
                "INSERT INTO t_serial VALUES (2, 2);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t_simple VALUES (2, 3);",
                "INSERT INTO t_composite VALUES (2, 2, 3);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 3);",
                "INSERT INTO t_serial VALUES (2, 3);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 3);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 34; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 3);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 35; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'other', 't_simple');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'other', 't_composite');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'other', 't_array');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'other', 't_serial');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'other', 't_generated');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'other', 't_trigger');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'other', 't_default_func');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: the Go server's output was wrong here, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'other', 'f_default()');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "f_default()" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                // Doltgrust-specific: the Go server's output was wrong here, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'other', 'f_trigger()');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "f_trigger()" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                // Doltgrust-specific: the Go server's output was wrong here, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS('main', 'other', 't_serial_pk_seq');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "t_serial_pk_seq" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_preview_merge_conflicts_summary() {
    run_scripts(&[
        ScriptTest {
            name: "Preview summary",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, v1 INT4, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY, v1 INT4);",
                "CREATE TABLE t_generated (pk INT8 PRIMARY KEY, v1 INT4, v2 INT8 GENERATED ALWAYS AS (pk * 1000) STORED);",
                "CREATE TABLE t_trigger (pk INT4 PRIMARY KEY, v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (pk INT4 PRIMARY KEY, v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1, 1);",
                "INSERT INTO t_composite VALUES (1, 1, 1);",
                "INSERT INTO t_array VALUES (ARRAY['abc'], 1);",
                "INSERT INTO t_serial VALUES (DEFAULT, 1);",
                "INSERT INTO t_generated (pk, v1) VALUES (1, 1);",
                "INSERT INTO t_trigger VALUES (1, 1);",
                "INSERT INTO t_default_func (pk, v2) VALUES (1, 1);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t_simple VALUES (2, 2);",
                "INSERT INTO t_composite VALUES (2, 2, 2);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 2);",
                "INSERT INTO t_serial VALUES (DEFAULT, 2);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t_simple VALUES (2, 3);",
                "INSERT INTO t_composite VALUES (2, 2, 3);",
                "INSERT INTO t_array VALUES (ARRAY['def'], 3);",
                "INSERT INTO t_serial VALUES (DEFAULT, 3);",
                "INSERT INTO t_generated (pk, v1) VALUES (2, 3);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 34; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger VALUES (2, 3);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 35; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (pk, v2) VALUES (2, 3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_PREVIEW_MERGE_CONFLICTS_SUMMARY('main', 'other');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_query_diff() {
    run_scripts(&[
        ScriptTest {
            name: "Smoke test",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY, v1 INT4);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('other');",
                "INSERT INTO t_simple VALUES (1, 1), (2, 1);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'next')::text) = 32;",
                "SELECT DOLT_CHECKOUT('other');",
                "INSERT INTO t_simple VALUES (1, 2), (2, 2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'next')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: the Go server's output was wrong here, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_QUERY_DIFF('SELECT * FROM t_simple AS OF main', 'SELECT * FROM t_simple AS OF other');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "main""#, position: 30, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_rm() {
    run_scripts(&[
        ScriptTest {
            name: "Cached only",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT DOLT_ADD('-A');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_RM('--cached', 't_simple','t_composite','t_array','t_serial','t_default_simple','t_checked','t_fk_parent','t_fk_child','t_unique','t_generated','t_trigger','t_default_func','f_trigger()','f_default()','t_serial_pk_seq','t_trigger.trig_trigger');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_rm", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16")],
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
fn test_dolt_reset() {
    run_scripts(&[
        ScriptTest {
            name: "Hard",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_RESET('--hard', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    skip: Some("Go skips this assertion until root objects are supported; untracked tables survive reset --hard, as in the Go server"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Soft",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT DOLT_ADD('-A');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_RESET('--soft', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the result STORAGE_DESIGN §IV.3a gives is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status WHERE staged = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("14")],
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
fn test_dolt_revert() {
    run_scripts(&[
        ScriptTest {
            name: "Revert a single commit",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "INSERT INTO t_simple VALUES (4);",
                "INSERT INTO t_composite VALUES (3, 100);",
                "INSERT INTO t_array VALUES (ARRAY['stu']);",
                "INSERT INTO t_serial VALUES (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (98, 8);",
                "INSERT INTO t_checked VALUES (99);",
                "INSERT INTO t_fk_parent VALUES (40);",
                "INSERT INTO t_fk_child VALUES (30);",
                "INSERT INTO t_unique VALUES (10);",
                "INSERT INTO t_generated (v1) VALUES (11);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger (v1) VALUES (2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (v2) VALUES (3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT f_default();",
                    expected: Expected::Rows {
                        columns: &[Column("f_default", INT8)],
                        rows: &[
                            &[T("34")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT substring(DOLT_REVERT('HEAD')::text, 34);",
                    expected: Expected::Rows {
                        columns: &[Column("substring", TEXT)],
                        rows: &[
                            &[T(",0,0,0)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t_simple;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT f_default();",
                    expected: Expected::Rows {
                        columns: &[Column("f_default", INT8)],
                        rows: &[
                            &[T("33")],
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
fn test_dolt_schema_diff() {
    run_scripts(&[
        ScriptTest {
            name: "Single commit",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "SELECT DOLT_BRANCH('original');",
                "INSERT INTO t_simple VALUES (4);",
                "INSERT INTO t_composite VALUES (3, 100);",
                "INSERT INTO t_array VALUES (ARRAY['stu']);",
                "INSERT INTO t_serial VALUES (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (98, 8);",
                "INSERT INTO t_checked VALUES (99);",
                "INSERT INTO t_fk_parent VALUES (40);",
                "INSERT INTO t_fk_child VALUES (30);",
                "INSERT INTO t_unique VALUES (10);",
                "INSERT INTO t_generated (v1) VALUES (11);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger (v1) VALUES (2);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (v2) VALUES (3);",
                "ALTER TABLE t_simple RENAME COLUMN pk TO rcol;",
                "ALTER TABLE t_composite RENAME COLUMN pk1 TO rcol;",
                "ALTER TABLE t_array RENAME COLUMN pk TO rcol;",
                "ALTER TABLE t_serial RENAME COLUMN pk TO rcol;",
                "ALTER TABLE t_default_simple RENAME COLUMN v1 TO rcol;",
                "ALTER TABLE t_generated RENAME COLUMN v1 TO rcol;",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM DOLT_SCHEMA_DIFF('main', 'original');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
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
fn test_dolt_stash() {
    run_scripts(&[
        ScriptTest {
            name: "Push and pop all untracked",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_STASH('push', 'dgstash', '--all');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_stash", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_STASH('pop', 'dgstash');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_stash", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("16")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Push and pop tracked",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_composite (pk1 INT2, pk2 INT8, PRIMARY KEY(pk1, pk2));",
                "CREATE TABLE t_array (pk TEXT[] PRIMARY KEY);",
                "CREATE TABLE t_serial (pk SERIAL PRIMARY KEY);",
                "CREATE TABLE t_default_simple (v1 INT8 DEFAULT 22, v2 INT8);",
                "CREATE TABLE t_checked (v1 NUMERIC CHECK (v1 > 0 AND v1 <= 100));",
                "CREATE TABLE t_fk_parent (pk INT4 PRIMARY KEY);",
                "CREATE TABLE t_fk_child (pk INT4 REFERENCES t_fk_parent(pk), PRIMARY KEY(pk));",
                "CREATE TABLE t_unique (v1 INT4 UNIQUE);",
                "CREATE TABLE t_generated (v1 INT8, v2 INT8 GENERATED ALWAYS AS (v1 * 1000) STORED);",
                "CREATE TABLE t_trigger (v1 INT8);",
                "CREATE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 3; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "CREATE TRIGGER trig_trigger BEFORE INSERT OR UPDATE ON t_trigger FOR EACH ROW EXECUTE FUNCTION f_trigger();",
                "CREATE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 33; END; $$ LANGUAGE plpgsql;",
                "CREATE TABLE t_default_func (v1 INT8 DEFAULT f_default(), v2 INT8);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "INSERT INTO t_composite VALUES (1, 100), (1, 101), (2, 100), (2, 101);",
                "INSERT INTO t_array VALUES (ARRAY['abc']), (ARRAY['def','ghi']), (ARRAY['jkl','mno','pqr']);",
                "INSERT INTO t_serial VALUES (DEFAULT), (DEFAULT), (DEFAULT);",
                "INSERT INTO t_default_simple VALUES (DEFAULT, 5), (99, 6), (DEFAULT, 7);",
                "INSERT INTO t_checked VALUES (1), (50), (100);",
                "INSERT INTO t_fk_parent VALUES (10), (20), (30);",
                "INSERT INTO t_fk_child VALUES (10), (20);",
                "INSERT INTO t_unique VALUES (7), (8), (9);",
                "INSERT INTO t_generated (v1) VALUES (1), (2), (10);",
                "CREATE OR REPLACE FUNCTION f_trigger() RETURNS TRIGGER AS $$ BEGIN NEW.v1 := NEW.v1 * 33; RETURN NEW; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_trigger (v1) VALUES (5), (10), (0);",
                "CREATE OR REPLACE FUNCTION f_default() RETURNS INT8 AS $$ BEGIN RETURN 34; END; $$ LANGUAGE plpgsql;",
                "INSERT INTO t_default_func (v2) VALUES (1), (2);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("15")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_STASH('push', 'dgstash');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_stash", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_STASH('pop', 'dgstash');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: r#"error: Your local changes to the following tables would be overwritten by applying stash 0:
"#, message_contains: true, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
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
fn test_dolt_tag() {
    run_scripts(&[
        ScriptTest {
            name: "Smoke test",
            set_up_script: &[
                "CREATE TABLE t_simple (pk INT4 PRIMARY KEY);",
                "INSERT INTO t_simple VALUES (1), (2), (3);",
                "SELECT length(DOLT_COMMIT('-A', '-m', 'initial')::text) = 32;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_TAG('tagged_commit', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('tagged_commit');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: r#"dolt does not support a detached head state. To create a branch at this tag, run: 
	CALL DOLT_CHECKOUT('tagged_commit', '-b', <new_branch_name>)"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_verify_constraints() {
    run_scripts(&[
        ScriptTest {
            name: "Foreign key violations from a merge",
            set_up_script: &[
                "CREATE TABLE p (id INT PRIMARY KEY);",
                "CREATE TABLE c (id INT PRIMARY KEY, pid INT REFERENCES p(id));",
                "INSERT INTO p VALUES (1), (2);",
                "SELECT DOLT_COMMIT('-Am', 'init');",
                "SELECT DOLT_CHECKOUT('-b', 'other');",
                "DELETE FROM p WHERE id = 2;",
                "SELECT DOLT_COMMIT('-am', 'del');",
                "SELECT DOLT_CHECKOUT('main');",
                "INSERT INTO c VALUES (1, 2);",
                "SELECT DOLT_COMMIT('-am', 'child');",
                "SET dolt_force_transaction_commit = on;",
                "SELECT DOLT_MERGE('other');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_VERIFY_CONSTRAINTS('p');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_VERIFY_CONSTRAINTS('c');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: the Go server returns 0 because it checks no tables without table names.
                ScriptTestAssertion {
                    query: "SELECT DOLT_VERIFY_CONSTRAINTS();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_c;",
                    expected: Expected::Rows {
                        columns: &[],
                        rows: &[],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_VERIFY_CONSTRAINTS('--all', 'c');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", NUMERIC)],
                        rows: &[
                            &[T("c"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_VERIFY_CONSTRAINTS('nope');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "table not found: nope", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_writes_after_dolt_gc() {
    run_scripts(&[
        ScriptTest {
            name: "writes after DOLT_GC reference chunks in the old generation",
            set_up_script: &[
                "CREATE TABLE t (a INT4 PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT DOLT_COMMIT('-A', '-m', 'first');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_GC();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (b INT4);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
    ]);
}

#[test]
fn test_create_index_after_dolt_gc() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE INDEX after DOLT_GC removed the empty map",
            set_up_script: &[
                "CREATE TABLE gi (id INT4 PRIMARY KEY, v INT4);",
                "INSERT INTO gi SELECT i, i % 7 FROM generate_series(1, 50) i;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT DOLT_GC();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX gi_v ON gi (v);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO gi VALUES (51, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM gi WHERE v = 3 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("10")],
                            &[T("17")],
                            &[T("24")],
                            &[T("31")],
                            &[T("38")],
                            &[T("45")],
                            &[T("51")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_commit_author_forms() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_commit authors read as Dolt reads them",
            set_up_script: &[
                "CREATE TABLE au (id INT PRIMARY KEY);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the expected output is Dolt's behavior.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-Am', 'first', '--author', 'Alice Author <alice@example.com')::text) > 0;",
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
                    query: "SELECT committer, email, message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("Alice Author"), T("alice@example.com"), T("first")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO au VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'second', '--author', 'Alice Author <>');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "Aborting commit due to empty author email. Is your config set?", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'third', '--author', 'Alice');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "Author not formatted correctly. Use 'Name <author@example.com>' format", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_table_renames_and_checkouts() {
    run_scripts(&[
        ScriptTest {
            name: "renamed tables in the status and staging",
            set_up_script: &[
                "CREATE TABLE r (id INT PRIMARY KEY, v INT);",
                "INSERT INTO r VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'base');",
                "ALTER TABLE r RENAME TO r2;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.r -> public.r2"), T("f"), T("renamed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('r');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.r -> public.r2"), T("f"), T("renamed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('r2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.r -> public.r2"), T("t"), T("renamed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping the last view drops dolt_schemas",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY);",
                "CREATE VIEW vv AS SELECT id FROM t;",
                "SELECT dolt_commit('-Am', 'base');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP VIEW vv;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.dolt_schemas"), T("f"), T("deleted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checking out every table of a commit",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'base');",
                "UPDATE t SET v = 5;",
                "SELECT dolt_add('t');",
                "UPDATE t SET v = 8;",
                "CREATE TABLE extra (id INT PRIMARY KEY);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_checkout('HEAD', '.')::text) > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.extra"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('HEAD', 'missing');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "tablespec 'missing' did not match any table(s) known to dolt", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "blame ignores temporary tables",
            set_up_script: &[
                "CREATE TABLE b (id INT PRIMARY KEY, v TEXT);",
                "INSERT INTO b VALUES (1, 'a');",
                "SELECT dolt_commit('-Am', 'INIT');",
                "UPDATE b SET v = 'b' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'UPDATE');",
                "CREATE TEMP TABLE b (x TEXT PRIMARY KEY, y INT);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT id, message FROM dolt_blame_b;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("message", TEXT)],
                        rows: &[
                            &[T("1"), T("UPDATE")],
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
fn test_dolt_hashof_db_and_table() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_hashof_db and dolt_hashof_table",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'c');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_db() = dolt_hashof_db('HEAD'), dolt_hashof_db('main') = dolt_hashof_db('HEAD'), dolt_hashof_db('STAGED') = dolt_hashof_db();",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_hashof_table('t'));",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4)],
                        rows: &[
                            &[T("32")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_db() = dolt_hashof_db('HEAD'), dolt_hashof_db('WORKING') = dolt_hashof_db();",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_table('nope');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "table not found: nope", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_schema_conflicts_roll_back_autocommit() {
    run_scripts(&[
        ScriptTest {
            name: "a merge with schema conflicts rolls back under autocommit",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'ancestor');",
                "SELECT dolt_branch('feat');",
                "SELECT dolt_checkout('feat');",
                "ALTER TABLE t ADD COLUMN v TEXT;",
                "SELECT dolt_commit('-Am', 'feat_add_text');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN v INTEGER;",
                "SELECT dolt_commit('-Am', 'main_add_int');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('feat');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT is_merging FROM dolt_merge_status;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_status;",
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
fn test_dolt_procedure_null_arguments() {
    run_scripts(&[
        ScriptTest {
            name: "Dolt's procedures read NULL arguments as empty strings",
            set_up_script: &[
                "CREATE TABLE u (id INT PRIMARY KEY);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_clean(NULL);",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "failed to clean; table not found: ''", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_commit_validations() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_commit refuses what Dolt refuses",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'base');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', '');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Aborting commit due to empty commit message.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t RENAME TO t2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'nothing stageable');",
                    expected: Expected::Rows { columns: &[Column("dolt_commit", TEXT)], rows: &[&[Any]], tag: "SELECT 1" },
                    dolt: Some(Dolt::Differs("Dolt's -a skips a renamed table and says nothing to commit, where Doltgrust commits the rename as one change of a tracked table (Appendix C, dolt_commit('-am') with an unstaged rename)")),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt's -a skips a renamed table and says nothing to commit, where Doltgrust commits the rename as one change of a tracked table (Appendix C, dolt_commit('-am') with an unstaged rename)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE parents (id INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE children (id INT PRIMARY KEY, pid INT REFERENCES parents (id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('children');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "NOTICE", code: "00000", message: "dolt_add also stages public.parents", ..N }],
                    dolt: Some(Dolt::Differs("Doltgrust's staging keeps commits valid, so staging children also stages parents, the new table its foreign key references, where Dolt stages children alone and its commit refuses the reference (STORAGE_DESIGN, staging keeps commits valid)")),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'child only');",
                    expected: Expected::Rows { columns: &[Column("dolt_commit", TEXT)], rows: &[&[Any]], tag: "SELECT 1" },
                    dolt: Some(Dolt::Differs("Doltgrust's staging keeps commits valid, so staging children also stages parents, the new table its foreign key references, where Dolt stages children alone and its commit refuses the reference (STORAGE_DESIGN, staging keeps commits valid)")),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-f', '-m', 'child only forced')) > 0;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "nothing to commit", ..E }),
                    dolt: Some(Dolt::Differs("Doltgrust's staging keeps commits valid, so staging children also stages parents, the new table its foreign key references, where Dolt stages children alone and its commit refuses the reference (STORAGE_DESIGN, staging keeps commits valid), so the commit before took both tables and a forced commit has nothing left")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_reset_tables() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_reset with several tables resets them all, and table names fold to lower case",
            set_up_script: &[
                "CREATE TABLE a (id INT PRIMARY KEY);",
                "CREATE TABLE b (id INT PRIMARY KEY);",
                "CREATE TABLE Camel (id INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO a VALUES (1);",
                "INSERT INTO b VALUES (1);",
                "INSERT INTO camel VALUES (1);",
                "SELECT dolt_add('-A');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('a', 'b');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged FROM dolt_status ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL)],
                        rows: &[
                            &[T("public.a"), T("f")],
                            &[T("public.b"), T("f")],
                            &[T("public.camel"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('Camel');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged FROM dolt_status ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL)],
                        rows: &[
                            &[T("public.a"), T("f")],
                            &[T("public.b"), T("f")],
                            &[T("public.camel"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('CAMEL');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged FROM dolt_status ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL)],
                        rows: &[
                            &[T("public.a"), T("f")],
                            &[T("public.b"), T("f")],
                            &[T("public.camel"), T("f")],
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
fn test_dolt_rebase_columns() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_rebase names its result columns",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_checkout('-b', 'feat');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'f1');",
                "SELECT dolt_checkout('main');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'm1');",
                "SELECT dolt_checkout('feat');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_rebase('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Successfully rebased and updated refs/heads/feat")],
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
fn test_status_leaves_out_ignored_tables() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_status leaves out new tables that dolt_ignore ignores",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('secret', true);",
                "CREATE TABLE secret (x INT PRIMARY KEY);",
                "CREATE TABLE kept (x INT PRIMARY KEY);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_ignore"), T("f"), T("new table")],
                            &[T("public.kept"), T("f"), T("new table")],
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
fn test_recreated_table_keeps_tags() {
    run_scripts(&[
        ScriptTest {
            name: "a table dropped and created again keeps its columns' tags, so its changes merge",
            set_up_script: &[
                "CREATE TABLE t (id INTEGER PRIMARY KEY, v INT, w INT, c INT);",
                "INSERT INTO t VALUES (1, 1, 1, 1);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_checkout('-b', 'feat');",
                "DROP TABLE t;",
                "CREATE TABLE t (id INTEGER PRIMARY KEY, v INT, w INT, c INT, CONSTRAINT ck CHECK (v > 0));",
                "INSERT INTO t VALUES (1, 1, 1, 1);",
                "SELECT dolt_commit('-Am', 'check');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t DROP COLUMN c;",
                "SELECT dolt_commit('-Am', 'drop');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts, message FROM dolt_merge('feat');",
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT count(*) FROM information_schema.columns WHERE table_name = 't'), (SELECT count(*) FROM information_schema.check_constraints WHERE constraint_name = 'ck'), id, v, w FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8), Column("id", INT4), Column("v", INT4), Column("w", INT4)],
                        rows: &[
                            &[T("3"), T("1"), T("1"), T("1"), T("1")],
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
fn test_workspace_table_changes() {
    run_scripts(&[
        ScriptTest {
            name: "UPDATE of a workspace table stages and unstages rows, and DELETE discards working changes",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(20) UNIQUE, w INT);",
                "INSERT INTO t VALUES (1, 'a', 1), (2, 'b', 1), (3, 'c', 1);",
                "SELECT dolt_commit('-Am', 'base');",
                "UPDATE t SET v = 'tmp' WHERE id = 1;",
                "UPDATE t SET v = 'a' WHERE id = 2;",
                "UPDATE t SET v = 'b' WHERE id = 1;",
                "UPDATE t SET w = 5 WHERE id = 3;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_t SET dolt_staged = true WHERE to_id <= 2;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_id, dolt_staged, dolt_diff_type, to_id, to_v, from_id, from_v FROM dolt_workspace_t ORDER BY dolt_id;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_id", INT8), Column("dolt_staged", BOOL), Column("dolt_diff_type", TEXT), Column("to_id", INT4), Column("to_v", VARCHAR), Column("from_id", INT4), Column("from_v", VARCHAR)],
                        rows: &[
                            &[T("0"), T("t"), T("modified"), T("1"), T("b"), T("1"), T("a")],
                            &[T("1"), T("t"), T("modified"), T("2"), T("a"), T("2"), T("b")],
                            &[T("2"), T("f"), T("modified"), T("3"), T("c"), T("3"), T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' wording for the error.
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_t SET dolt_staged = 1 WHERE to_id = 3;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "column \"dolt_staged\" is of type boolean but expression is of type integer", hint: "You will need to rewrite or cast the expression.", position: 43, ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_t SET to_w = 9 WHERE to_id = 3;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "only update of column 'dolt_staged' is allowed", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_workspace_t WHERE dolt_staged;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot delete staged rows from workspace", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_workspace_t WHERE to_id = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("w", INT4)],
                        rows: &[
                            &[T("1"), T("b"), T("1")],
                            &[T("2"), T("a"), T("1")],
                            &[T("3"), T("c"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "UPDATE dolt_workspace_t SET dolt_staged = false WHERE to_id = 1;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"t_v_key\"", detail: "Key (v)=(a) already exists.", schema: "public", table: "t", constraint: "t_v_key", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("t"), T("modified")],
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
fn test_merge_computes_generated_columns() {
    run_scripts(&[
        ScriptTest {
            name: "a merge computes stored generated columns from the merged values",
            set_up_script: &[
                "CREATE TABLE g (id INT PRIMARY KEY, x INT, y INT, z INT GENERATED ALWAYS AS (x + y) STORED);",
                "INSERT INTO g (id, x, y) VALUES (1, 1, 1), (2, 2, NULL);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('feature');",
                "UPDATE g SET x = 2 WHERE id = 1;",
                "ALTER TABLE g ADD COLUMN d INT GENERATED ALWAYS AS (x * 10) STORED;",
                "SELECT dolt_commit('-Am', 'left');",
                "SELECT dolt_checkout('feature');",
                "UPDATE g SET y = 3 WHERE id = 1;",
                "UPDATE g SET y = 5 WHERE id = 2;",
                "INSERT INTO g (id, x, y) VALUES (3, 7, 8);",
                "SELECT dolt_commit('-Am', 'right');",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts, message FROM dolt_merge('feature');",
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM g ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("x", INT4), Column("y", INT4), Column("z", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), T("5"), T("20")],
                            &[T("2"), T("2"), T("5"), T("7"), T("20")],
                            &[T("3"), T("7"), T("8"), T("15"), T("70")],
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
fn test_dolt_tests_and_test_run() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_tests holds test queries that DOLT_TEST_RUN runs and checks",
            set_up_script: &[
                "CREATE TABLE fixture (i INT PRIMARY KEY, s TEXT, d DATE, n NUMERIC(10,2), b BOOL);",
                "INSERT INTO fixture VALUES (1, 'one', '2025-08-22', 10.4, true), (2, 'two', NULL, NULL, NULL);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_tests;",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group", TEXT), Column("test_query", TEXT), Column("assertion_type", TEXT), Column("assertion_comparator", TEXT), Column("assertion_value", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run();",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "could not find tests for argument: *", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' wording for the error.
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_tests VALUES ('bad', NULL, 'SELECT 1', 'row_count', '==', '1');",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"dolt_tests\" violates check constraint \"assertion_type_check\"", detail: "Failing row contains (bad, null, SELECT 1, row_count, ==, 1).", schema: "public", table: "dolt_tests", constraint: "assertion_type_check", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: r#"INSERT INTO dolt_tests VALUES
						('a_rows', 'g', 'SELECT i FROM fixture', 'expected_rows', '==', '3'),
						('b_cols', 'g', 'SELECT i, s FROM fixture LIMIT 1', 'expected_columns', '==', '2'),
						('c_date', 'types', 'SELECT d FROM fixture WHERE i = 1', 'expected_single_value', '>', '2025-08-23'),
						('d_numeric', 'types', 'SELECT n FROM fixture WHERE i = 1', 'expected_single_value', '>', '10.5'),
						('e_bool', 'types', 'SELECT b FROM fixture WHERE i = 1', 'expected_single_value', '==', 'true'),
						('f_null', NULL, 'SELECT NULL', 'expected_single_value', '!=', NULL),
						('g_rows', 'shape', 'SELECT i FROM fixture', 'expected_single_value', '==', '1'),
						('h_two', 'checks', 'SELECT 1; SELECT 2', 'expected_rows', '==', '1'),
						('i_write', 'checks', 'CREATE TABLE nope (x INT)', 'expected_rows', '==', '1'),
						('j_missing', 'checks', 'SELECT * FROM absent', 'expected_rows', '==', '0');"#,
                    expected: Expected::Tag("INSERT 0 10"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT test_name, test_group_name, status, message FROM dolt_test_run();",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("test_group_name", TEXT), Column("status", TEXT), Column("message", TEXT)],
                        rows: &[
                            &[T("a_rows"), T("g"), T("FAIL"), T("Assertion failed: expected_rows equal to 3, got 2")],
                            &[T("b_cols"), T("g"), T("PASS"), T("")],
                            &[T("c_date"), T("types"), T("FAIL"), T("Assertion failed: expected_single_value greater than 2025-08-23, got 2025-08-22")],
                            &[T("d_numeric"), T("types"), T("FAIL"), T("Assertion failed: expected_single_value greater than 10.5, got 10.40")],
                            &[T("e_bool"), T("types"), T("PASS"), T("")],
                            &[T("f_null"), T(""), T("FAIL"), T("Assertion failed: expected_single_value not equal to NULL, got NULL")],
                            &[T("g_rows"), T("shape"), T("FAIL"), T("expected_single_value expects exactly one cell. Received multiple rows")],
                            &[T("h_two"), T("checks"), T("FAIL"), T("Can only run exactly one query")],
                            &[T("i_write"), T("checks"), T("FAIL"), T("Cannot execute write queries")],
                            &[T("j_missing"), T("checks"), T("FAIL"), T("query error: relation \"absent\" does not exist")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT test_name, status FROM dolt_test_run('types', 'b_cols');",
                    expected: Expected::Rows {
                        columns: &[Column("test_name", TEXT), Column("status", TEXT)],
                        rows: &[
                            &[T("c_date"), T("FAIL")],
                            &[T("d_numeric"), T("FAIL")],
                            &[T("e_bool"), T("PASS")],
                            &[T("b_cols"), T("PASS")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_test_run('nothing');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "could not find tests for argument: nothing", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_schema_merge_dropped_columns() {
    run_scripts(&[
        ScriptTest {
            name: "a merge drops the other side's index of a dropped column and conflicts on its edits of one",
            set_up_script: &[
                "CREATE TABLE t (k INT PRIMARY KEY, a VARCHAR(9), b VARCHAR(9), n INT);",
                "INSERT INTO t VALUES (1, 'a1', 'b1', 11), (2, 'a2', 'b2', 22);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('feat');",
                "ALTER TABLE t DROP COLUMN a;",
                "SELECT dolt_commit('-Am', 'ours');",
                "SELECT dolt_checkout('feat');",
                "CREATE INDEX ia ON t (a);",
                "UPDATE t SET b = 'edit' WHERE k = 1;",
                "SELECT dolt_commit('-Am', 'theirs');",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts, message FROM dolt_merge('feat');",
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("b", VARCHAR), Column("n", INT4)],
                        rows: &[
                            &[T("1"), T("edit"), T("11")],
                            &[T("2"), T("b2"), T("22")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexname FROM pg_indexes WHERE tablename = 't' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("t_pkey")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('feat2', 'HEAD~1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP COLUMN b;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT length(dolt_commit('-Am', 'drop b')) > 0) AND length(dolt_checkout('feat2')::text) > 0;",
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
                    query: "UPDATE t SET b = 'edit2' WHERE k = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT length(dolt_commit('-Am', 'edit b')) > 0) AND length(dolt_checkout('main')::text) > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts, message FROM dolt_merge('feat2');",
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT \"table\", num_conflicts FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", NUMERIC)],
                        rows: &[
                            &[T("t"), T("1")],
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
fn test_merge_column_type_changes() {
    run_scripts(&[
        ScriptTest {
            name: "a merge converts rows for type changes with implicit or assignment casts and conflicts on others",
            set_up_script: &[
                "CREATE TABLE t (k INT PRIMARY KEY, v INT, w VARCHAR(5), x TEXT);",
                "INSERT INTO t VALUES (1, 1, 'a', '1'), (2, 2, 'b', '2');",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('rows');",
                "SELECT dolt_branch('explicit');",
                "SELECT dolt_branch('more_rows');",
                "ALTER TABLE t ALTER COLUMN v TYPE TEXT;",
                "ALTER TABLE t ALTER COLUMN w TYPE VARCHAR(10);",
                "SELECT dolt_commit('-Am', 'widen');",
                "SELECT dolt_checkout('rows');",
                "UPDATE t SET w = 'c', v = 7 WHERE k = 2;",
                "SELECT dolt_commit('-Am', 'rows');",
                "SELECT dolt_checkout('explicit');",
                "ALTER TABLE t ALTER COLUMN x TYPE INT USING x::int;",
                "SELECT dolt_commit('-Am', 'explicit');",
                "SELECT dolt_checkout('more_rows');",
                "UPDATE t SET x = '9' WHERE k = 1;",
                "SELECT dolt_commit('-Am', 'more rows');",
                "SELECT dolt_checkout('main');",
                "CREATE TABLE n (k INT PRIMARY KEY, y INT8);",
                "INSERT INTO n VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'n');",
                "SELECT dolt_branch('big');",
                "ALTER TABLE n ALTER COLUMN y TYPE INT4;",
                "SELECT dolt_commit('-Am', 'narrow');",
                "SELECT dolt_checkout('big');",
                "INSERT INTO n VALUES (2, 5000000000);",
                "SELECT dolt_commit('-Am', 'big');",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts, message FROM dolt_merge('rows');",
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, v, w FROM t ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("v", TEXT), Column("w", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("a")],
                            &[T("2"), T("7"), T("c")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts, message FROM dolt_merge('big');",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "integer out of range", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('more_rows');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T("(0,\"Switched to branch 'more_rows'\")")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts, message FROM dolt_merge('explicit');",
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
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
    ]);
}

#[test]
fn test_merge_new_column_edits_and_schema_conflicts() {
    run_scripts(&[
        ScriptTest {
            name: "an edit of a column the base lacks leaves a delete alone, and dolt_conflicts lists schema conflicts",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY, b INT);",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
                "CREATE TABLE s (id INT PRIMARY KEY, a INT);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_checkout('-b', 'feature');",
                "ALTER TABLE t ADD COLUMN d TEXT DEFAULT 'x';",
                "DELETE FROM t WHERE a = 2;",
                "ALTER TABLE s ADD COLUMN extra INT;",
                "SELECT dolt_commit('-Am', 'feature');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c INT DEFAULT 7;",
                "UPDATE t SET c = 8 WHERE a = 2;",
                "ALTER TABLE s ADD COLUMN extra TEXT;",
                "SELECT dolt_commit('-Am', 'main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts, message FROM dolt_merge('feature');",
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4), Column("d", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("7"), T("x")],
                            &[T("3"), T("3"), T("7"), T("x")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT \"table\", num_conflicts FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", NUMERIC)],
                        rows: &[
                            &[T("s"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("s")],
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
            ],
            ..S
        },
    ]);
}

#[test]
fn test_merge_checks_and_aborts() {
    run_scripts(&[
        ScriptTest {
            name: "a merge records check violations, keeps root object conflicts, and its abort keeps ignored tables",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a INT, b INT, CONSTRAINT ab CHECK (a <= b));",
                "INSERT INTO t VALUES (1, 1, 5), (2, 1, 5);",
                "CREATE TABLE items (id INT PRIMARY KEY, label TEXT);",
                "INSERT INTO items VALUES (1, 'base');",
                "INSERT INTO dolt_ignore VALUES ('runtime_%', true);",
                "CREATE TABLE audit (a INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'base');",
                "CREATE TABLE runtime_jobs (id INT PRIMARY KEY, kind TEXT);",
                "INSERT INTO runtime_jobs VALUES (1, 'keep');",
                "SELECT dolt_checkout('-b', 'side');",
                "UPDATE t SET a = 4 WHERE id = 1;",
                "UPDATE items SET label = 'side' WHERE id = 1;",
                "CREATE FUNCTION f() RETURNS trigger AS $$ BEGIN INSERT INTO audit VALUES (NEW.id); RETURN NEW; END $$ LANGUAGE plpgsql;",
                "SELECT dolt_commit('-Am', 'side');",
                "SELECT dolt_checkout('main');",
                "UPDATE t SET b = 2 WHERE id = 1;",
                "CREATE FUNCTION f() RETURNS trigger AS $$ BEGIN INSERT INTO audit VALUES (NEW.id + 100); RETURN NEW; END $$ LANGUAGE plpgsql;",
                "SELECT dolt_commit('-Am', 'main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts, message FROM dolt_merge('side');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "Committing this transaction resulted in a working set with constraint violations, transaction rolled back. This constraint violation may be the result of a previous merge or the result of transaction sequencing. Constraint violations from a merge can be resolved using the dolt_constraint_violations table before committing the transaction. To allow transactions to be committed with constraint violations from a merge or transaction sequencing set dolt_force_transaction_commit to on.\nConstraint violations: \nType: Check Constraint Violation,\n\tName: ab,\n\tExpression: a <= b", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts, message FROM dolt_merge('side');",
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, id, a, b FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", VARCHAR), Column("id", INT4), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("4"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE runtime_jobs SET kind = 'updated' WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_merge", RECORD)],
                        rows: &[
                            &[T("(\"\",0,0,\"merge aborted\")")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM runtime_jobs;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("kind", TEXT)],
                        rows: &[
                            &[T("1"), T("updated")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM items;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("label", TEXT)],
                        rows: &[
                            &[T("1"), T("base")],
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
fn test_docs_agent_updates_and_aliases() {
    run_scripts(&[
        ScriptTest {
            name: "an UPDATE of the default AGENT.md stores it, and table arguments accept dolt_docs",
            set_up_script: &[],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "UPDATE dolt_docs SET doc_text = 'x' WHERE doc_name = 'README.md';",
                    expected: Expected::Tag("UPDATE 0"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_docs WHERE doc_text LIKE '# AGENT.md%';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("Doltgrust carries no AGENT.md, so dolt_docs holds only stored docs (the owner, 2026-10-31)"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "UPDATE dolt_docs SET doc_text = 'edited' WHERE doc_name = 'AGENT.md';",
                    expected: Expected::Tag("UPDATE 1"),
                    skip: Some("Doltgrust carries no AGENT.md, so dolt_docs holds only stored docs (the owner, 2026-10-31)"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT doc_name, doc_text FROM dolt_docs;",
                    expected: Expected::Rows {
                        columns: &[Column("doc_name", TEXT), Column("doc_text", TEXT)],
                        rows: &[
                            &[T("AGENT.md"), T("edited")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("Doltgrust carries no AGENT.md, so dolt_docs holds only stored docs (the owner, 2026-10-31)"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('dolt_docs');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Rust server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt.docs"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("Doltgrust carries no AGENT.md, so dolt_docs holds only stored docs (the owner, 2026-10-31)"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_hashof_three_dot_log_reset_and_detached_sessions() {
    run_scripts(&[
        ScriptTest {
            name: "hashof matches names without case, logs take three-dot ranges, reset checks conflicts, and detached sessions limit tags",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, v TEXT);",
                "INSERT INTO t VALUES (1, 'a');",
                "SELECT dolt_commit('-Am', 'init');",
                "SELECT dolt_tag('v1');",
                "SELECT dolt_branch('Feat');",
                "SELECT dolt_checkout('Feat');",
                "INSERT INTO t VALUES (2, 'f');",
                "SELECT dolt_commit('-Am', 'feat');",
                "SELECT dolt_checkout('main');",
                "INSERT INTO t VALUES (3, 'm');",
                "SELECT dolt_commit('-Am', 'main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('MAIN') = dolt_hashof('main'), dolt_hashof('feat') = dolt_hashof('Feat');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log('main...Feat') ORDER BY message;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("feat")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = 'main' WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT length(dolt_commit('-Am', 'main edit')) > 0) AND length(dolt_checkout('Feat')::text) > 0;",
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
                    query: "UPDATE t SET v = 'feat' WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT length(dolt_commit('-Am', 'feat edit')) > 0) AND length(dolt_checkout('main')::text) > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts FROM dolt_merge('Feat');",
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "RESET dolt_allow_commit_conflicts;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_reset();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "USE postgres/v1",
                    expected: Expected::Tag("SET"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('blocked');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "this operation is not supported while in a detached head state", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('v1');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "dolt does not support a detached head state. To create a branch at this tag, run: \n\tCALL DOLT_CHECKOUT('v1', '-b', <new_branch_name>)", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_gc();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
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
fn test_status_renames_foreign_keys_and_history_as_of() {
    run_scripts(&[
        ScriptTest {
            name: "status leaves a referenced table alone, table resets follow renames, and history tables resolve AS OF",
            set_up_script: &[
                "CREATE TABLE parent (id INT PRIMARY KEY);",
                "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
                "SELECT dolt_commit('-Am', 'base');",
                "CREATE TABLE child (id INT PRIMARY KEY, parent_id INT REFERENCES parent (id));",
                "SELECT dolt_add('child');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.child"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'child')) > 0;",
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
                    query: "ALTER TABLE t RENAME TO t2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t2'), dolt_reset('t2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8), Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t -> public.t2"), T("f"), T("renamed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Bug("Dolt leaves the rename staged after dolt_reset of its new name, which resets only its old name; resetting either name unstages it")),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t -> public.t2"), T("f"), T("renamed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_diff_summary('HEAD~1', 'HEAD', NULL);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Invalid argument to dolt_diff_summary: NULL", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT length(dolt_checkout('-b', 'feature')::text) > 0;",
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
                    query: "CREATE TABLE feature_only (id INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO feature_only VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT (SELECT length(dolt_commit('-Am', 'feature only')) > 0) AND length(dolt_checkout('main')::text) > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT id FROM dolt_history_feature_only AS OF 'feature';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
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
fn test_merge_foreign_keys_and_schema_conflicts() {
    run_scripts(&[
        ScriptTest {
            name: "a merge keeps the foreign keys both sides added and refuses keys both sides changed differently",
            set_up_script: &[
                "CREATE TABLE parent (id INT PRIMARY KEY);",
                "CREATE TABLE a (id INT PRIMARY KEY, pid INT);",
                "CREATE TABLE b (id INT PRIMARY KEY, pid INT);",
                "CREATE TABLE c (id INT PRIMARY KEY, pid INT REFERENCES parent (id));",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_checkout('-b', 'feat');",
                "ALTER TABLE b ADD CONSTRAINT b_fk FOREIGN KEY (pid) REFERENCES parent (id);",
                "SELECT dolt_commit('-Am', 'feat fk');",
                "SELECT dolt_checkout('-b', 'feat2', 'main');",
                "ALTER TABLE c DROP CONSTRAINT c_pid_fkey;",
                "ALTER TABLE c ADD CONSTRAINT c_pid_fkey FOREIGN KEY (pid) REFERENCES parent (id) ON DELETE CASCADE;",
                "SELECT dolt_commit('-Am', 'cascade');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE a ADD CONSTRAINT a_fk FOREIGN KEY (pid) REFERENCES parent (id);",
                "ALTER TABLE c DROP CONSTRAINT c_pid_fkey;",
                "ALTER TABLE c ADD CONSTRAINT c_pid_fkey FOREIGN KEY (pid) REFERENCES parent (id) ON DELETE SET NULL;",
                "SELECT dolt_commit('-Am', 'main fks');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts, message FROM dolt_merge('feat');",
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname FROM pg_constraint WHERE contype = 'f' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME)],
                        rows: &[
                            &[T("a_fk")],
                            &[T("b_fk")],
                            &[T("c_pid_fkey")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts, message FROM dolt_merge('feat2');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "foreign key conflicts", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_conflicts_resolve refuses schema conflicts, and dolt_verify_constraints matches tables without regard to case",
            set_up_script: &[
                "CREATE TABLE s (id INT PRIMARY KEY, a INT);",
                "CREATE TABLE \"MiXeD\" (id INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_checkout('-b', 'feature');",
                "ALTER TABLE s ADD COLUMN extra INT;",
                "SELECT dolt_commit('-Am', 'feature');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE s ADD COLUMN extra TEXT;",
                "SELECT dolt_commit('-Am', 'main');",
                "INSERT INTO \"MiXeD\" VALUES (1);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('mixed');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('MIXED');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "error: the table(s) MIXED do not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts FROM dolt_merge('feature');",
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 's');",
                    expected: Expected::Error(Diagnostic {
                        code: "XX000",
                        message: "Unable to automatically resolve schema conflicts since data changes may not have been fully merged yet. To continue, abort this merge (dolt merge --abort) then apply ALTER TABLE statements to one side of this merge to get the two schemas in sync with the desired schema, then rerun the merge. To track resolution of this limitation, follow https://github.com/dolthub/dolt/issues/6616",
                        ..E
                    }),
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
            name: "a merge refuses two new foreign keys with one name on different tables",
            set_up_script: &[
                "CREATE TABLE parent (id INT PRIMARY KEY);",
                "CREATE TABLE parent2 (id INT PRIMARY KEY);",
                "CREATE TABLE child (id INT PRIMARY KEY, pid INT);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_checkout('-b', 'feat');",
                "ALTER TABLE child ADD FOREIGN KEY (pid) REFERENCES parent (id);",
                "SELECT dolt_commit('-Am', 'feat');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE child ADD FOREIGN KEY (pid) REFERENCES parent2 (id);",
                "SELECT dolt_commit('-Am', 'main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts FROM dolt_merge('feat');",
                    expected: Expected::Error(Diagnostic {
                        code: "XX000",
                        message: "duplicate foreign key constraint name `child_pid_fkey`",
                        ..E
                    }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_schema_conflicts shows a table one side dropped as <deleted>",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, v INT);",
                "CREATE TABLE u (id INT PRIMARY KEY, v INT);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_checkout('-b', 'feature');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "INSERT INTO u VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'feature');",
                "SELECT dolt_checkout('main');",
                "DROP TABLE t;",
                "DROP TABLE u;",
                "SELECT dolt_commit('-Am', 'main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT conflicts FROM dolt_merge('feature');",
                    expected: Expected::Rows {
                        columns: &[Column("conflicts", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT table_name, our_schema, description FROM dolt_schema_conflicts ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("our_schema", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("<deleted>"), T("cannot merge a table deletion with schema modification")],
                            &[T("public.u"), T("<deleted>"), T("cannot merge a table deletion with data modification")],
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
            ],
            ..S
        },
    ]);
}

#[test]
fn test_verify_constraints_reverifies_unique_indexes() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_verify_constraints records unique index violations again among changed rows or all rows",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY, col1 INT UNIQUE);",
                "SELECT dolt_commit('-Am', 'init');",
                "SELECT dolt_branch('branch1');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'insert on main');",
                "SELECT dolt_checkout('branch1');",
                "INSERT INTO t VALUES (2, 1);",
                "SELECT dolt_commit('-Am', 'insert on branch1');",
                "SELECT dolt_checkout('main');",
                "SET dolt_force_transaction_commit = 1;",
                "BEGIN;",
                "SELECT dolt_merge('branch1');",
                "COMMIT;",
                "DELETE FROM dolt_constraint_violations_t;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, col1, dolt_violation_info::text FROM dolt_constraint_violations_t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", VARCHAR), Column("pk", INT4), Column("col1", INT4), Column("dolt_violation_info", TEXT)],
                        rows: &[
                            &[T("unique index"), T("1"), T("1"), T("{\"Columns\":[\"col1\"],\"Name\":\"t_col1_key\"}")],
                            &[T("unique index"), T("2"), T("1"), T("{\"Columns\":[\"col1\"],\"Name\":\"t_col1_key\"}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--all', '--output-only');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_verify_constraints('--all');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_verify_constraints", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
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
fn test_dolt_rebase_keeps_every_pick() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_rebase replays each commit of the plan on the one before it",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_checkout('-b', 'feat');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'f1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'f2');",
                "SELECT dolt_checkout('main');",
                "INSERT INTO t VALUES (3);",
                "SELECT dolt_commit('-am', 'm1');",
                "SELECT dolt_checkout('feat');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_rebase('main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_rebase", RECORD)],
                        rows: &[
                            &[T("(0,\"Successfully rebased and updated refs/heads/feat\")")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 4;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("f2")],
                            &[T("f1")],
                            &[T("m1")],
                            &[T("base")],
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
fn test_new_branch_indexes() {
    run_scripts(&[
        ScriptTest {
            name: "a branch made from a commit finds rows through its indexes",
            set_up_script: &[
                "CREATE TABLE q (id INT PRIMARY KEY, name TEXT UNIQUE);",
                "CREATE INDEX q_lower ON q (lower(name));",
                "INSERT INTO q VALUES (1, 'One'), (2, 'Two');",
                "SELECT dolt_commit('-Am', 'rows');",
                "SELECT dolt_branch('b');",
                "SELECT dolt_checkout('b');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT id FROM q WHERE name = 'Two';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT id FROM q WHERE lower(name) = 'one';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES (3, 'One');",
                    expected: Expected::Error(Diagnostic {
                        code: "23505",
                        message: "duplicate key value violates unique constraint \"q_name_key\"",
                        detail: "Key (name)=(One) already exists.",
                        schema: "public",
                        table: "q",
                        constraint: "q_name_key",
                        ..E
                    }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_status_objects() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_status names objects other than tables kind first",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v text);",
                "SELECT dolt_add('t');",
                "CREATE TABLE u (pk int PRIMARY KEY);",
                "CREATE SCHEMA s;",
                "CREATE FUNCTION f(i integer) RETURNS integer LANGUAGE sql AS 'SELECT i';",
                "CREATE SEQUENCE seq;",
                "CREATE TABLE w (id serial PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("t"), T("new table")],
                            &[T("function public.f(integer)"), T("f"), T("new function")],
                            &[T("public.u"), T("f"), T("new table")],
                            &[T("public.w"), T("f"), T("new table")],
                            &[T("sequence public.seq"), T("f"), T("new sequence")],
                            &[T("schema s"), T("f"), T("new schema")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-Am', 'first')) > 0;",
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO w DEFAULT VALUES;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u RENAME TO u2;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION f;",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('seq');",
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("function public.f(integer)"), T("f"), T("deleted")],
                            &[T("public.u -> public.u2"), T("f"), T("renamed")],
                            &[T("public.w"), T("f"), T("modified")],
                            &[T("sequence public.seq"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 4",
                    },
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA s;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("function public.f(integer)"), T("t"), T("deleted")],
                            &[T("public.u -> public.u2"), T("t"), T("renamed")],
                            &[T("public.w"), T("t"), T("modified")],
                            &[T("sequence public.seq"), T("t"), T("modified")],
                            &[T("public.t"), T("f"), T("modified")],
                            &[T("schema s"), T("f"), T("deleted schema")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_status reads dropped columns and tables from the committed root",
            set_up_script: &[
                "CREATE TABLE a (x int PRIMARY KEY, y int, z int);",
                "CREATE TABLE b (x int PRIMARY KEY, y int);",
                "ALTER TABLE b DROP COLUMN y;",
                "ALTER TABLE b ADD COLUMN w text;",
                "SELECT dolt_commit('-Am', 'one');",
                "ALTER TABLE a DROP COLUMN y;",
                "DROP TABLE b;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.a"), T("f"), T("modified")],
                            &[T("public.b"), T("f"), T("deleted")],
                        ],
                        tag: "SELECT 2",
                    },
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
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.a"), T("t"), T("modified")],
                            &[T("public.b"), T("t"), T("deleted")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_add stages what a table's parts need, and a foreign key's referenced table only when it is new",
            set_up_script: &[
                "CREATE TABLE p (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'p');",
                "INSERT INTO p VALUES (1);",
                "CREATE FUNCTION trig() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END; $$;",
                "CREATE TABLE t (v int);",
                "CREATE TRIGGER tr BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION trig();",
                "CREATE TABLE c (id int PRIMARY KEY REFERENCES p (id));",
                "CREATE TABLE p2 (id int PRIMARY KEY);",
                "CREATE TABLE c2 (id int REFERENCES p2 (id));",
                "CREATE FUNCTION g(i integer) RETURNS integer LANGUAGE sql AS 'SELECT i';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "dolt_add also stages function public.trig()", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, status FROM dolt_status WHERE staged;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("status", TEXT)],
                        rows: &[
                            &[T("function public.trig()"), T("new function")],
                            &[T("public.t"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('c', 'c2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "dolt_add also stages public.p2", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("function public.trig()"), T("t"), T("new function")],
                            &[T("public.c"), T("t"), T("new table")],
                            &[T("public.c2"), T("t"), T("new table")],
                            &[T("public.p2"), T("t"), T("new table")],
                            &[T("public.t"), T("t"), T("new table")],
                            &[T("function public.g(integer)"), T("f"), T("new function")],
                            &[T("public.p"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('function g(integer)');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_status WHERE NOT staged;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.p")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('function nope(integer)');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function nope(integer) does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_transaction_commit_prepared() {
    run_scripts(&[
        ScriptTest {
            name: "PREPARE TRANSACTION refuses a transaction whose commit would make a Dolt commit",
            server_config: r#"system_variables:
  max_prepared_transactions: 10
"#,
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SET dolt_transaction_commit = on;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE TRANSACTION 'p1';",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "dolt_transaction_commit is not supported with PREPARE TRANSACTION yet", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM t;",
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
            name: "PREPARE TRANSACTION makes the Dolt commit dolt_transaction_commit asks for, which COMMIT PREPARED publishes",
            server_config: r#"system_variables:
  max_prepared_transactions: 10
"#,
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SET dolt_transaction_commit = on;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE TRANSACTION 'p1';",
                    expected: Expected::Tag("PREPARE TRANSACTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("CREATE DATABASE")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT PREPARED 'p1';",
                    expected: Expected::Tag("COMMIT PREPARED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Transaction commit")],
                            &[T("CREATE DATABASE")],
                            &[T("Initialize data repository")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("2f core: prepared transactions' ref updates (VC_PLAN A.10)"),
            ..S
        },
    ]);
}

#[test]
fn test_dolt_reset_hard_install() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_reset --hard writes a table's rows without firing its triggers",
            set_up_script: &[
                "CREATE TABLE trg (x int);",
                "CREATE FUNCTION tf() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE 'fired'; RETURN COALESCE(NEW, OLD); END; $$;",
                "CREATE TRIGGER tt BEFORE INSERT OR UPDATE OR DELETE ON trg FOR EACH ROW EXECUTE FUNCTION tf();",
                "INSERT INTO trg VALUES (1);",
                "SELECT dolt_commit('-Am', 'base');",
                "INSERT INTO trg VALUES (2);",
                "UPDATE trg SET x = 10 WHERE x = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM trg;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
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
            name: "dolt_reset --hard across RENAME COLUMN keeps the column's attribute number",
            set_up_script: &[
                "CREATE TABLE rn (a int PRIMARY KEY, b int, c int);",
                "INSERT INTO rn VALUES (1, 2, 3);",
                "SELECT dolt_commit('-Am', 'base');",
                "ALTER TABLE rn RENAME COLUMN b TO bb;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attnum, attisdropped FROM pg_attribute WHERE attrelid = 'rn'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnum", INT2), Column("attisdropped", BOOL)],
                        rows: &[
                            &[T("a"), T("1"), T("f")],
                            &[T("b"), T("2"), T("f")],
                            &[T("c"), T("3"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rn;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reset --hard across DROP and ADD of a column of one name keeps the new column's attribute number",
            set_up_script: &[
                "CREATE TABLE rn (a int PRIMARY KEY, b int, c int);",
                "INSERT INTO rn VALUES (1, 2, 3);",
                "SELECT dolt_commit('-Am', 'base');",
                "ALTER TABLE rn DROP COLUMN c;",
                "ALTER TABLE rn ADD COLUMN c int;",
                "UPDATE rn SET c = 30;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attnum, attisdropped FROM pg_attribute WHERE attrelid = 'rn'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnum", INT2), Column("attisdropped", BOOL)],
                        rows: &[
                            &[T("a"), T("1"), T("f")],
                            &[T("b"), T("2"), T("f")],
                            &[T("........pg.dropped.3........"), T("3"), T("t")],
                            &[T("c"), T("4"), T("f")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rn;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reset --hard brings back a dropped column at its attribute number",
            set_up_script: &[
                "CREATE TABLE rn (a int PRIMARY KEY, b int, c int);",
                "INSERT INTO rn VALUES (1, 2, 3);",
                "SELECT dolt_commit('-Am', 'base');",
                "ALTER TABLE rn DROP COLUMN b;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, attnum, attisdropped FROM pg_attribute WHERE attrelid = 'rn'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnum", INT2), Column("attisdropped", BOOL)],
                        rows: &[
                            &[T("a"), T("1"), T("f")],
                            &[T("b"), T("2"), T("f")],
                            &[T("c"), T("3"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rn;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reset --hard leaves a table only the working state has as it is",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2, 20);",
                "CREATE TABLE u (x int);",
                "INSERT INTO u VALUES (5);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.u"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reset --hard restores rows, enum labels, functions, views, foreign keys, and large values",
            set_up_script: &[
                "CREATE TYPE mood AS ENUM ('a', 'b');",
                "CREATE TABLE k (x int, y text);",
                "INSERT INTO k VALUES (1, 'a'), (1, 'a'), (2, 'b');",
                "CREATE TABLE s (id int PRIMARY KEY, m mood);",
                "INSERT INTO s VALUES (1, 'a'), (2, 'b');",
                "CREATE TABLE p (id int PRIMARY KEY);",
                "CREATE TABLE c (id int PRIMARY KEY, pid int REFERENCES p (id));",
                "INSERT INTO p VALUES (1), (2);",
                "INSERT INTO c VALUES (10, 1);",
                "CREATE VIEW v AS SELECT id FROM s;",
                "CREATE FUNCTION f(i int) RETURNS int LANGUAGE sql AS 'SELECT i + 1';",
                "CREATE TABLE d (id int PRIMARY KEY DEFERRABLE, v int);",
                "INSERT INTO d VALUES (1, 1), (2, 2);",
                "CREATE TABLE big (id int PRIMARY KEY, t text);",
                "INSERT INTO big VALUES (1, repeat('x', 100000));",
                "SELECT dolt_commit('-Am', 'base');",
                "INSERT INTO k VALUES (1, 'a'), (3, 'c');",
                "DELETE FROM k WHERE x = 2;",
                "DELETE FROM s WHERE id = 1;",
                "ALTER TYPE mood ADD VALUE 'c';",
                "DROP VIEW v;",
                "CREATE OR REPLACE FUNCTION f(i int) RETURNS int LANGUAGE sql AS 'SELECT i + 2';",
                "DELETE FROM c;",
                "DELETE FROM p WHERE id = 1;",
                "UPDATE d SET v = 20 WHERE id = 2;",
                "INSERT INTO d VALUES (3, 3);",
                "UPDATE big SET t = repeat('y', 100000);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
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
                ScriptTestAssertion {
                    query: "SELECT * FROM k ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT enum_range(NULL::mood)::text;",
                    expected: Expected::Rows {
                        columns: &[Column("enum_range", TEXT)],
                        rows: &[
                            &[T("{a,b}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT f(1);",
                    expected: Expected::Rows {
                        columns: &[Column("f", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("pid", INT4)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM d ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(t), substr(t, 1, 3) FROM big;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("substr", TEXT)],
                        rows: &[
                            &[T("100000"), T("xxx")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c VALUES (11, 99);",
                    expected: Expected::Error(Diagnostic {
                        code: "23503",
                        message: "insert or update on table \"c\" violates foreign key constraint \"c_pid_fkey\"",
                        detail: "Key (pid)=(99) is not present in table \"p\".",
                        schema: "public",
                        table: "c",
                        constraint: "c_pid_fkey",
                        ..E
                    }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_reset --hard leaves a sequence's values where they are when they are ahead of the commit's",
            set_up_script: &[
                "CREATE TABLE s (id serial PRIMARY KEY);",
                "INSERT INTO s DEFAULT VALUES;",
                "SELECT dolt_commit('-Am', 'base');",
                "INSERT INTO s DEFAULT VALUES;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s;",
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('s_id_seq');",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
                        rows: &[
                            &[T("3")],
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
fn test_dolt_checkout_tables() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_checkout of a table gives it the staged root's rows and leaves other tables alone",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "CREATE TABLE u (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_add('t');",
                "INSERT INTO t VALUES (3, 30);",
                "INSERT INTO u VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('t');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T("(0,\"\")")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u;",
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("t"), T("modified")],
                            &[T("public.u"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout after -- brings back a dropped table from HEAD",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "CREATE TABLE u (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_add('t');",
                "INSERT INTO t VALUES (3, 30);",
                "INSERT INTO u VALUES (1);",
                "DROP TABLE u;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('--', 'u');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T("(0,\"\")")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("t"), T("modified")],
                            &[T("public.t"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout of . gives every tracked object its staged state and leaves untracked tables alone",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "CREATE TABLE u (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_add('t');",
                "INSERT INTO t VALUES (3, 30);",
                "INSERT INTO u VALUES (1);",
                "CREATE TABLE w (x int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('.');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T("(0,\"\")")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("t"), T("modified")],
                            &[T("public.w"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_checkout fails for a name no root has, stays on its own branch, and makes and switches to a new branch",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "CREATE TABLE u (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'c1');",
                "INSERT INTO t VALUES (2, 20);",
                "SELECT dolt_add('t');",
                "INSERT INTO t VALUES (3, 30);",
                "INSERT INTO u VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('nope');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "error: tablespec 'nope' did not match any table(s) known to dolt", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T("(0,\"Already on branch 'main'\")")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'x');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T("(0,\"Switched to branch 'x'\")")],
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
fn test_dolt_first_commit_identity() {
    run_scripts(&[
        ScriptTest {
            name: "a database's first commit and a dolt_commit sign with the same identity",
            intent: "CREATE DATABASE's first commit is signed by the session's user and email, as each dolt_commit of that session is.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'first');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*), count(DISTINCT (committer, email, author, author_email)) FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("2"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT committer, email LIKE 'postgres@%' FROM dolt_log WHERE message = 'CREATE DATABASE';",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("?column?", BOOL)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_foreign_key_triggers_belong_to_their_table() {
    run_scripts(&[
        ScriptTest {
            name: "a foreign key's triggers on the referenced table belong to the table that declares it",
            intent: "A foreign key added to z changes only z in dolt_status, dolt_checkout of z takes the key and its triggers on t away and brings them back, and the key's triggers fire both ways throughout.",
            set_up_script: &[
                "CREATE TABLE t (c1 int PRIMARY KEY);",
                "CREATE TABLE z (c1 int PRIMARY KEY, c2 int);",
                "SELECT dolt_commit('-Am', 'create tables t and z');",
                "ALTER TABLE z ADD CONSTRAINT fk FOREIGN KEY (c1) REFERENCES t (c1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.z"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgrelid::regclass::text, count(*) FROM pg_trigger WHERE tgconstraint = (SELECT oid FROM pg_constraint WHERE conname = 'fk') GROUP BY 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("tgrelid", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("t"), T("2")],
                            &[T("z"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO z VALUES (5, 1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"z\" violates foreign key constraint \"fk\"", detail: "Key (c1)=(5) is not present in table \"t\".", schema: "public", table: "z", constraint: "fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO z VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t;",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "update or delete on table \"t\" violates foreign key constraint \"fk\" on table \"z\"", detail: "Key (c1)=(1) is still referenced from table \"z\".", schema: "public", table: "z", constraint: "fk", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('z');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("f"), T("modified")],
                            &[T("public.z"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('z');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tgrelid::regclass::text, count(*) FROM pg_trigger WHERE tgconstraint = (SELECT oid FROM pg_constraint WHERE conname = 'fk') GROUP BY 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("tgrelid", TEXT), Column("count", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE z ADD CONSTRAINT fk FOREIGN KEY (c1) REFERENCES t (c1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'add fk');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE z DROP CONSTRAINT fk;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.z"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('z');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
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
                ScriptTestAssertion {
                    query: "SELECT tgrelid::regclass::text, count(*) FROM pg_trigger WHERE tgconstraint = (SELECT oid FROM pg_constraint WHERE conname = 'fk') GROUP BY 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("tgrelid", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("t"), T("2")],
                            &[T("z"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO z VALUES (5, 1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"z\" violates foreign key constraint \"fk\"", detail: "Key (c1)=(5) is not present in table \"t\".", schema: "public", table: "z", constraint: "fk", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_functions_take_bound_parameters() {
    run_scripts(&[
        ScriptTest {
            name: "Doltgrust's version control functions run as prepared statements with bound arguments",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_add($1);",
                    bind_vars: &[BindVar::Str("t")],
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit($1, $2);",
                    bind_vars: &[BindVar::Str("-m"), BindVar::Str("prepared commit")],
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log($1) LIMIT 1;",
                    bind_vars: &[BindVar::Str("HEAD")],
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("prepared commit")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof($1) = dolt_hashof('HEAD');",
                    bind_vars: &[BindVar::Str("HEAD")],
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_table($1) IS NOT NULL;",
                    bind_vars: &[BindVar::Str("t")],
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_db($1) IS NOT NULL;",
                    bind_vars: &[BindVar::Str("HEAD")],
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch($1);",
                    bind_vars: &[BindVar::Str("feature")],
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_branches WHERE name = $1;",
                    bind_vars: &[BindVar::Str("feature")],
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout($1);",
                    bind_vars: &[BindVar::Str("feature")],
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature'")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch() = $1;",
                    bind_vars: &[BindVar::Str("feature")],
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES ($1);",
                    bind_vars: &[BindVar::Str("2")],
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_status WHERE table_name = $1;",
                    bind_vars: &[BindVar::Str("public.t")],
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset($1);",
                    bind_vars: &[BindVar::Str("--hard")],
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_status WHERE table_name = $1;",
                    bind_vars: &[BindVar::Str("public.t")],
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_creating_a_role_alone_leaves_nothing_to_commit() {
    run_scripts(&[
        ScriptTest {
            name: "a role change alone leaves nothing to commit",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE committed_role;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's error is expected, as roles live outside branches.
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-Am', 'roles');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "nothing to commit", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_undrop_and_purge() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_undrop and dolt_purge_dropped_databases",
            set_up_script: &[
                "CREATE DATABASE d1;",
                "DROP DATABASE d1;",
                "CREATE DATABASE d1;",
                "COMMENT ON DATABASE d1 IS 'second';",
                "CREATE DATABASE d2;",
                "DROP DATABASE d1;",
                "DROP DATABASE d2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no database name specified. available databases that can be undropped: d1, d1, d2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop('D2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no database named 'D2' found to undrop. available databases that can be undropped: d1, d1, d2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop('d1', 'd2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "dolt_undrop called with too many arguments: dolt_undrop only accepts one argument - the name of the dropped database to restore", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_purge_dropped_databases('x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "dolt_purge_dropped_databases does not take any arguments", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop('d2');",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "dolt_undrop cannot run inside a transaction block", ..E }),
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
                    query: "SELECT dolt_purge_dropped_databases();",
                    expected: Expected::Error(Diagnostic { code: "25001", message: "dolt_purge_dropped_databases cannot run inside a transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop('d1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_undrop", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no database name specified. available databases that can be undropped: d1, d2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop('d1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to undrop database 'd1'; another database already exists with the same name", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE newer;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop('d2');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "the databases' roles differ, which behavior.server_wide_roles does not allow", detail: "Role \"newer\" is in database \"postgres\" but not in database \"d2\".", hint: "Start the server with behavior.server_wide_roles: false, make the roles match in every database, then turn it back on.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE datname LIKE 'd_' ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("d1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no database name specified. available databases that can be undropped: d1, d2", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE newer;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop('d2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_undrop", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname FROM pg_database WHERE datname LIKE 'd_' ORDER BY datname;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME)],
                        rows: &[
                            &[T("d1")],
                            &[T("d2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_purge_dropped_databases();",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_purge_dropped_databases", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_undrop();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no database name specified. there are no databases currently available to be undropped", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT shobj_description(oid, 'pg_database') FROM pg_database WHERE datname = 'd1';",
                    expected: Expected::Rows {
                        columns: &[Column("shobj_description", TEXT)],
                        rows: &[
                            &[T("second")],
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
fn test_dolt_diff_commit_hash_filters() {
    run_scripts(&[
        ScriptTest {
            name: "commit_hash filters of dolt_diff and dolt_column_diff",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY, v INT);",
                "SELECT dolt_commit('-Am', 'create t');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'on other');",
                "SELECT set_config('test.other', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'on main');",
                "SELECT set_config('test.main', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t VALUES (3, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT message, table_name FROM dolt_diff WHERE commit_hash = current_setting('test.main');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("table_name", TEXT)],
                        rows: &[
                            &[T("on main"), T("public.t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_diff WHERE current_setting('test.main') = commit_hash AND table_name = 'public.t';",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("on main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_diff WHERE commit_hash IN (current_setting('test.main'), current_setting('test.other'));",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("on main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_diff WHERE commit_hash = current_setting('test.other');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash, table_name FROM dolt_diff WHERE commit_hash = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("commit_hash", TEXT), Column("table_name", TEXT)],
                        rows: &[
                            &[T("WORKING"), T("public.t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_diff WHERE commit_hash = 'working';",
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
                    query: "SELECT count(*) FROM dolt_diff WHERE commit_hash = 'nope';",
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
                    query: "SELECT message FROM dolt_diff WHERE commit_hash = current_setting('test.other') OR table_name = 'zzz';",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, diff_type FROM dolt_column_diff WHERE commit_hash = current_setting('test.main') ORDER BY column_name;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("pk"), T("modified")],
                            &[T("v"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_column_diff WHERE commit_hash = current_setting('test.other');",
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
                    query: "SELECT column_name FROM dolt_column_diff WHERE commit_hash = 'WORKING' ORDER BY column_name;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", TEXT)],
                        rows: &[
                            &[T("pk")],
                            &[T("v")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT message FROM dolt_diff WHERE commit_hash = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on pg_catalog.dolt_diff")],
                            &[T("  Output: dolt_diff.message")],
                            &[T("  Function Call: pg_catalog.dolt_diff(1, 3, false, 'WORKING'::text)")],
                            &[T("  Filter: (dolt_diff.commit_hash = 'WORKING'::text)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_diff AS OF 'HEAD' WHERE commit_hash = current_setting('test.main');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("on main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_column_diff AS OF 'HEAD~1' WHERE commit_hash = current_setting('test.main');",
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
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT message FROM dolt_diff AS OF 'HEAD' WHERE commit_hash = 'WORKING';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on pg_catalog.dolt_diff")],
                            &[T("  Output: dolt_diff.message")],
                            &[T("  Function Call: pg_catalog.dolt_diff('HEAD'::text, 1, 3, false, 'WORKING'::text)")],
                            &[T("  Filter: (dolt_diff.commit_hash = 'WORKING'::text)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_commit_all_leaves_a_new_versioned_system_table_unstaged() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_commit -a leaves dolt_ignore alone until it is committed once",
            intent: "dolt_commit -a stages only the tables HEAD has, so a dolt_ignore that HEAD never held stays a new table, as Dolt's -a leaves it.",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'table');",
                "INSERT INTO dolt_ignore VALUES ('x*', true);",
                "INSERT INTO t VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'tracked only') IS NOT NULL;",
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_ignore"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_ignore AS OF 'HEAD';",
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
                    query: "SELECT * FROM t AS OF 'HEAD';",
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
    ]);
}

#[test]
fn test_stash_pop_of_keyless_changes_the_working_set_already_has() {
    run_scripts(&[
        ScriptTest {
            name: "a stash pop whose keyless row the working set already added applies",
            intent: "A stash pop takes a row of a table without a primary key that the working set holds as the stash does as already applied, while the working set's other changes stay.",
            set_up_script: &[
                "CREATE TABLE k (a INT);",
                "INSERT INTO k VALUES (1);",
                "SELECT dolt_commit('-Am', 'base');",
                "INSERT INTO k VALUES (2);",
                "SELECT dolt_stash('push', 's');",
                "INSERT INTO k VALUES (2);",
                "INSERT INTO k VALUES (3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 's');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM k ORDER BY a;",
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
                    query: "SELECT count(*) FROM dolt_stashes;",
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
            name: "a stash pop whose keyless row the working set holds in another number of copies conflicts",
            intent: "A stash pop still refuses a row of a table without a primary key that the stash and the working set changed to different numbers of copies, keeping the stash.",
            set_up_script: &[
                "CREATE TABLE k (a INT);",
                "INSERT INTO k VALUES (1);",
                "SELECT dolt_commit('-Am', 'base');",
                "INSERT INTO k VALUES (2);",
                "SELECT dolt_stash('push', 's');",
                "INSERT INTO k VALUES (2), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 's');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: Your local changes to the following tables would be overwritten by applying stash 0:\n\t{'k'}\nPlease commit your changes or stash them before you merge.\nAborting\nThe stash entry is kept in case you need it again.\n", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_stashes;",
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
    ]);
}

#[test]
fn test_catalog_changes_by_version_control_of_another_session() {
    run_scripts(&[ScriptTest {
        name: "a staged table another session's hard reset removed, and one of another branch, are gone for the next statement",
        set_up_script: &["CREATE TABLE vr (a INT);", "SELECT dolt_commit('-Am', 'base');"],
        assertions: &[
            ScriptTestAssertion { query: "CREATE TABLE vs (a INT);", expected: Expected::Tag("CREATE TABLE"), client: "b", ..A },
            ScriptTestAssertion {
                query: "SELECT dolt_add('vs');",
                expected: Expected::Rows { columns: &[Column("dolt_add", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                client: "b",
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM vs;",
                expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT dolt_reset('--hard');",
                expected: Expected::Rows { columns: &[Column("dolt_reset", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                client: "b",
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM vs;",
                expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "vs" does not exist"#, position: 22, ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT dolt_checkout('-b', 'other');",
                expected: Expected::Rows {
                    columns: &[Column("dolt_checkout", RECORD)],
                    rows: &[&[T("(0,\"Switched to branch 'other'\")")]],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion { query: "CREATE TABLE vo (a INT);", expected: Expected::Tag("CREATE TABLE"), ..A },
            ScriptTestAssertion {
                query: "SELECT dolt_checkout('main');",
                expected: Expected::Rows {
                    columns: &[Column("dolt_checkout", RECORD)],
                    rows: &[&[T("(0,\"Switched to branch 'main'\")")]],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM vo;",
                expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "vo" does not exist"#, position: 22, ..E }),
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_merge_holds_back_what_depends_on_a_conflicting_definition() {
    run_scripts(&[
        ScriptTest {
            name: "a table whose definitions conflict holds back a view theirs added, which resolving with --theirs takes",
            intent: "A view theirs added on a column of a table whose definitions conflict is held back with the table, so the merged catalog stays whole, and resolving the conflict with --theirs takes it.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c INT;",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE VIEW v AS SELECT id, d FROM t;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("incompatible column types for column 'c': character varying(100) and integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("d", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("seed"), Null, Null],
                            &[T("2"), T("two"), T("4"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), Null],
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
            name: "a table whose definitions conflict holds back a view theirs added, which resolving with --ours leaves out",
            intent: "A view theirs added on a column of a table whose definitions conflict is held back with the table, so the merged catalog stays whole, and resolving the conflict with --ours leaves it out.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c INT;",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE VIEW v AS SELECT id, d FROM t;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("incompatible column types for column 'c': character varying(100) and integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("c", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v ORDER BY id;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"v\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a table whose definitions conflict holds back a foreign key onto a new unique index theirs added, which resolving with --theirs takes",
            intent: "A foreign key onto a new unique index theirs added on a column of a table whose definitions conflict is held back with the table, so the merged catalog stays whole, and resolving the conflict with --theirs takes it.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c INT;",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE UNIQUE INDEX t_d ON t (d);",
                "CREATE TABLE u (id INT PRIMARY KEY, d INT REFERENCES t (d));",
                "INSERT INTO u VALUES (1, 4);",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("incompatible column types for column 'c': character varying(100) and integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("d", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("seed"), Null, Null],
                            &[T("2"), T("two"), T("4"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a table whose definitions conflict holds back a foreign key onto a new unique index theirs added, which resolving with --ours leaves out",
            intent: "A foreign key onto a new unique index theirs added on a column of a table whose definitions conflict is held back with the table, so the merged catalog stays whole, and resolving the conflict with --ours leaves it out.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c INT;",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE UNIQUE INDEX t_d ON t (d);",
                "CREATE TABLE u (id INT PRIMARY KEY, d INT REFERENCES t (d));",
                "INSERT INTO u VALUES (1, 4);",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("incompatible column types for column 'c': character varying(100) and integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("c", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"u\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a table whose definitions conflict holds back a sequence owned by a new column theirs added, which resolving with --theirs takes",
            intent: "A sequence owned by a new column theirs added on a column of a table whose definitions conflict is held back with the table, so the merged catalog stays whole, and resolving the conflict with --theirs takes it.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c INT;",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE SEQUENCE s OWNED BY t.d;",
                "SELECT nextval('s');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("incompatible column types for column 'c': character varying(100) and integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("d", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("seed"), Null, Null],
                            &[T("2"), T("two"), T("4"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('s');",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
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
            name: "a table whose definitions conflict holds back a sequence owned by a new column theirs added, which resolving with --ours leaves out",
            intent: "A sequence owned by a new column theirs added on a column of a table whose definitions conflict is held back with the table, so the merged catalog stays whole, and resolving the conflict with --ours leaves it out.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c INT;",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE SEQUENCE s OWNED BY t.d;",
                "SELECT nextval('s');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("incompatible column types for column 'c': character varying(100) and integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("c", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('s');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"s\" does not exist", position: 16, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a table whose definitions conflict holds back extended statistics on a new column theirs added, which resolving with --theirs takes",
            intent: "Extended statistics on a new column theirs added on a column of a table whose definitions conflict is held back with the table, so the merged catalog stays whole, and resolving the conflict with --theirs takes it.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c INT;",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE STATISTICS st (ndistinct) ON a, d FROM t;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("incompatible column types for column 'c': character varying(100) and integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("d", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("seed"), Null, Null],
                            &[T("2"), T("two"), T("4"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stxname FROM pg_statistic_ext;",
                    expected: Expected::Rows {
                        columns: &[Column("stxname", NAME)],
                        rows: &[
                            &[T("st")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a table whose definitions conflict holds back extended statistics on a new column theirs added, which resolving with --ours leaves out",
            intent: "Extended statistics on a new column theirs added on a column of a table whose definitions conflict is held back with the table, so the merged catalog stays whole, and resolving the conflict with --ours leaves it out.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c INT;",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE STATISTICS st (ndistinct) ON a, d FROM t;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("incompatible column types for column 'c': character varying(100) and integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("c", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stxname FROM pg_statistic_ext;",
                    expected: Expected::Rows {
                        columns: &[Column("stxname", NAME)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a column both sides added alike at different positions merges at ours' position",
            intent: "Both sides added column c with one definition at different positions, which converges as in Dolt, whose merge keeps ours' order of the columns.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "UPDATE t SET c = 'theirs';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT fast_forward, conflicts, message FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("d", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), Null, T("theirs")],
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
fn test_merge_resolve_with_theirs_refuses_what_depends_on_a_column_theirs_lack() {
    run_scripts(&[
        ScriptTest {
            name: "resolving a definition conflict with --theirs refuses while a view of ours depends on a column theirs lack",
            intent: "Resolving with --theirs would leave ours' a view depending on a column theirs' table lacks, so it refuses as Postgres' DROP COLUMN does without CASCADE, and succeeds once the dependency is gone.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN e TEXT;",
                "UPDATE t SET c = 99, e = 'theirs e';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE VIEW w AS SELECT id, d FROM t;",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot resolve the conflict of table \"t\" with --theirs because other objects depend on it", detail: "view w depends on column d of table t", hint: "Drop or change the objects that depend on it first.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW w;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("c", INT4), Column("e", TEXT)],
                        rows: &[
                            &[T("1"), T("seed"), T("99"), T("theirs e")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "resolving a definition conflict with --ours keeps a view of ours on a column theirs lack",
            intent: "Resolving with --ours keeps ours' table, so ours' a view on its column stays as it was.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN e TEXT;",
                "UPDATE t SET c = 99, e = 'theirs e';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE VIEW w AS SELECT id, d FROM t;",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("d", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), Null, Null],
                            &[T("2"), T("two"), T("4"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM w ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), Null],
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
            name: "resolving a definition conflict with --theirs refuses while a foreign key of ours depends on a column theirs lack",
            intent: "Resolving with --theirs would leave ours' a foreign key depending on a column theirs' table lacks, so it refuses as Postgres' DROP COLUMN does without CASCADE, and succeeds once the dependency is gone.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN e TEXT;",
                "UPDATE t SET c = 99, e = 'theirs e';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE UNIQUE INDEX t_d ON t (d);",
                "CREATE TABLE u (id INT PRIMARY KEY, d INT REFERENCES t (d));",
                "INSERT INTO u VALUES (1, 4);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot resolve the conflict of table \"t\" with --theirs because other objects depend on it", detail: "constraint u_d_fkey on table u depends on column d of table t", hint: "Drop or change the objects that depend on it first.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE u DROP CONSTRAINT u_d_fkey;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("c", INT4), Column("e", TEXT)],
                        rows: &[
                            &[T("1"), T("seed"), T("99"), T("theirs e")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "resolving a definition conflict with --ours keeps a foreign key of ours on a column theirs lack",
            intent: "Resolving with --ours keeps ours' table, so ours' a foreign key on its column stays as it was.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN e TEXT;",
                "UPDATE t SET c = 99, e = 'theirs e';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE UNIQUE INDEX t_d ON t (d);",
                "CREATE TABLE u (id INT PRIMARY KEY, d INT REFERENCES t (d));",
                "INSERT INTO u VALUES (1, 4);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("d", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), Null, Null],
                            &[T("2"), T("two"), T("4"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "resolving a definition conflict with --theirs refuses while a sequence it owns of ours depends on a column theirs lack",
            intent: "Resolving with --theirs would leave ours' a sequence it owns depending on a column theirs' table lacks, so it refuses as Postgres' DROP COLUMN does without CASCADE, and succeeds once the dependency is gone.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN e TEXT;",
                "UPDATE t SET c = 99, e = 'theirs e';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE SEQUENCE s OWNED BY t.d;",
                "SELECT nextval('s');",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot resolve the conflict of table \"t\" with --theirs because other objects depend on it", detail: "sequence s depends on column d of table t", hint: "Drop or change the objects that depend on it first.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SEQUENCE s OWNED BY NONE;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("c", INT4), Column("e", TEXT)],
                        rows: &[
                            &[T("1"), T("seed"), T("99"), T("theirs e")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "resolving a definition conflict with --ours keeps a sequence it owns of ours on a column theirs lack",
            intent: "Resolving with --ours keeps ours' table, so ours' a sequence it owns on its column stays as it was.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN e TEXT;",
                "UPDATE t SET c = 99, e = 'theirs e';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE SEQUENCE s OWNED BY t.d;",
                "SELECT nextval('s');",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("d", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), Null, Null],
                            &[T("2"), T("two"), T("4"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT nextval('s');",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
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
            name: "resolving a definition conflict with --theirs takes the table's extended statistics with it",
            intent: "Extended statistics belong to their table, as its indexes do, so resolving with --theirs takes theirs' table without ours' statistics on a column theirs lack.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN e TEXT;",
                "UPDATE t SET c = 99, e = 'theirs e';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN d INT;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "INSERT INTO t VALUES (2, 'two', 4, NULL);",
                "CREATE STATISTICS st (ndistinct) ON a, d FROM t;",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("c", INT4), Column("e", TEXT)],
                        rows: &[
                            &[T("1"), T("seed"), T("99"), T("theirs e")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT stxname FROM pg_statistic_ext;",
                    expected: Expected::Rows {
                        columns: &[Column("stxname", NAME)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "resolving a definition conflict with --theirs refuses while a view of ours uses a column whose type theirs changed",
            intent: "Resolving with --theirs changes the type of a column ours' view uses, so it refuses as Postgres' ALTER COLUMN TYPE does, and succeeds once the view is gone.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100), d INT);",
                "INSERT INTO t VALUES (1, 'seed', 4);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ALTER COLUMN d TYPE BIGINT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN d TYPE NUMERIC;",
                "CREATE VIEW w AS SELECT id, d FROM t;",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot alter type of a column used by a view or rule", detail: "rule _RETURN on view w depends on column \"d\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW w;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("d", INT8)],
                        rows: &[
                            &[T("1"), T("seed"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "resolving a definition conflict with --theirs refuses while another table stores the table's row type",
            intent: "Resolving with --theirs changes the columns of a table whose row type another table's column stores, so it refuses as Postgres refuses rewriting such a table, and succeeds once that table is gone.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "CREATE TABLE u (id INT PRIMARY KEY, r t);",
                "INSERT INTO t VALUES (1, 'seed');",
                "INSERT INTO u VALUES (1, ROW(1, 'seed'));",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c TEXT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot alter table \"t\" because column \"u.r\" uses its row type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE u;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("seed"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "resolving a definition conflict with --theirs refuses while a foreign key of ours can't join the column theirs retyped",
            intent: "Resolving with --theirs retypes a column ours' foreign key references to a type no equality operator joins with the referencing column's, so it refuses as Postgres' ALTER COLUMN TYPE does when it rebuilds the foreign key.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100), d INT UNIQUE);",
                "INSERT INTO t VALUES (1, 'seed', 4);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ALTER COLUMN d TYPE TEXT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN d TYPE BIGINT;",
                "CREATE TABLE u (id INT PRIMARY KEY, d BIGINT REFERENCES t (d));",
                "INSERT INTO u VALUES (1, 4);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "foreign key constraint \"u_d_fkey\" cannot be implemented", detail: "Key columns \"d\" of the referencing table and \"d\" of the referenced table are of incompatible types: bigint and text.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "resolving a definition conflict with --theirs keeps a foreign key of ours that still joins the column theirs retyped",
            intent: "Resolving with --theirs retypes a column ours' foreign key references to bigint, which an integer equality operator still joins with the referencing integer column, so it succeeds as Postgres' ALTER COLUMN TYPE rebuilds the foreign key.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100), d INT UNIQUE);",
                "INSERT INTO t VALUES (1, 'seed', 4);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ALTER COLUMN d TYPE BIGINT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN d TYPE NUMERIC;",
                "CREATE TABLE u (id INT PRIMARY KEY, d INT REFERENCES t (d));",
                "INSERT INTO u VALUES (1, 4);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "resolving a definition conflict with --theirs refuses while rows of ours no longer find the keys theirs retyped",
            intent: "Resolving with --theirs rounds the key ours' foreign key references, so a referencing row of ours finds no key and it refuses as Postgres' ALTER COLUMN TYPE does when it checks the rebuilt foreign key.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100), d NUMERIC UNIQUE);",
                "INSERT INTO t VALUES (1, 'seed', 1.5);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ALTER COLUMN d TYPE NUMERIC(3, 0);",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ALTER COLUMN d TYPE NUMERIC(10, 2);",
                "CREATE TABLE u (id INT PRIMARY KEY, d NUMERIC REFERENCES t (d));",
                "INSERT INTO u VALUES (1, 1.5);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"u\" violates foreign key constraint \"u_d_fkey\"", detail: "Key (d)=(1.5) is not present in table \"t\".", schema: "public", table: "u", constraint: "u_d_fkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "resolving a definition conflict with --theirs refuses while theirs' foreign key from the table can't join the key ours retyped",
            intent: "Resolving with --theirs takes theirs' foreign key from the table onto a key ours retyped to text, which no equality operator joins with its bigint column, so it refuses as Postgres' ALTER COLUMN TYPE does.",
            set_up_script: &[
                "CREATE TABLE parent (k BIGINT PRIMARY KEY);",
                "INSERT INTO parent VALUES (4);",
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN p BIGINT REFERENCES parent (k);",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c TEXT;",
                "ALTER TABLE parent ALTER COLUMN k TYPE TEXT;",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "foreign key constraint \"t_p_fkey\" cannot be implemented", detail: "Key columns \"p\" of the referencing table and \"k\" of the referenced table are of incompatible types: bigint and text.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "resolving a definition conflict with --theirs refuses while theirs' foreign key from the table names a column ours dropped",
            intent: "Resolving with --theirs takes theirs' foreign key from the table onto a column of another table that ours dropped, which would leave the constraint naming a missing column, so it refuses as the other dependents of a missing column do.",
            set_up_script: &[
                "CREATE TABLE parent (k BIGINT PRIMARY KEY, p BIGINT UNIQUE);",
                "INSERT INTO parent VALUES (4, 40);",
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN p BIGINT REFERENCES parent (p);",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE t ADD COLUMN c TEXT;",
                "ALTER TABLE parent DROP COLUMN p;",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot resolve the conflict of table \"t\" with --theirs because other objects depend on it", detail: "constraint t_p_fkey on table t depends on column p of table parent", hint: "Drop or change the objects that depend on it first.", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_merge_fills_a_volatile_default_once_per_row() {
    run_scripts(&[
        ScriptTest {
            name: "a merge fills a volatile default of a column one side added once for each row",
            intent: "One side added columns with random() and gen_random_uuid() defaults while the other changed and added rows, and the merge fills each row's values once, so the base's and the other side's copies of a row agree and nothing conflicts.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('y');",
                "ALTER TABLE t ADD COLUMN n FLOAT8 DEFAULT random();",
                "ALTER TABLE t ADD COLUMN u UUID DEFAULT gen_random_uuid();",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('y');",
                "INSERT INTO t VALUES (2, 'two');",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, n IS NOT NULL, u IS NOT NULL FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("1"), T("y edit"), T("t"), T("t")],
                            &[T("2"), T("two"), T("t"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
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
            name: "a merge fills a default that calls a function in the session once for each row, and a preview fills none",
            intent: "One side added columns with nextval and SQL function defaults, and the merge evaluates them in its session once for each row it converts, while a preview of it takes nothing from the sequence.",
            set_up_script: &[
                "CREATE SEQUENCE s;",
                "CREATE FUNCTION f() RETURNS INT LANGUAGE sql AS 'SELECT 7';",
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed'), (2, 'two');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('y');",
                "ALTER TABLE t ADD COLUMN n BIGINT DEFAULT nextval('s');",
                "ALTER TABLE t ADD COLUMN m INT DEFAULT f();",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('y');",
                "INSERT INTO t VALUES (3, 'three');",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'y');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT last_value FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("last_value", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("n", INT8), Column("m", INT4)],
                        rows: &[
                            &[T("1"), T("y edit"), T("1"), T("7")],
                            &[T("2"), T("two"), T("2"), T("7")],
                            &[T("3"), T("three"), T("5"), T("7")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT last_value FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("last_value", INT8)],
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
    ]);
}

#[test]
fn test_merge_of_a_column_both_sides_added() {
    run_scripts(&[
        ScriptTest {
            name: "a row of the merge base whose cells of a column both sides added differ conflicts",
            intent: "Both sides added column c and only ours set row 1's value in it, so the row conflicts on the cell, as Dolt's merge does.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('y');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "UPDATE t SET c = 5 WHERE id = 1;",
                "SELECT dolt_commit('-am', 'x');",
                "SELECT dolt_checkout('y');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'y');",
                "SELECT dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // The same DEFAULT on both sides fills equal values and doesn't conflict, and a different one is a schema
                // conflict. The same volatile DEFAULT, random(), stores different values in each side's own commits, so
                // every row of the merge base conflicts, as in Dolt.
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against Dolt main.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_id, base_a, our_a, our_c, their_a, their_c FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("our_c", INT4), Column("their_a", VARCHAR), Column("their_c", INT4)],
                        rows: &[
                            &[T("1"), T("seed"), T("seed"), T("5"), T("y edit"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rows each side added in a column both sides added merge cleanly",
            intent: "Both sides added column c and each added its own row with a value in it, rows the other side lacks, so the merge takes both, as Dolt's merge does.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('y');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "INSERT INTO t VALUES (2, 'two', 20);",
                "SELECT dolt_commit('-am', 'x');",
                "SELECT dolt_checkout('y');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "INSERT INTO t VALUES (3, 'three', 30);",
                "SELECT dolt_commit('-am', 'y');",
                "SELECT dolt_checkout('main');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against Dolt main.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("seed"), Null],
                            &[T("2"), T("two"), T("20")],
                            &[T("3"), T("three"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a column both sides added with different types is a schema conflict and no row conflicts",
            intent: "Both sides added column c, one as integer and one as text, so the definitions conflict and the table has no conflicts of rows.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('y');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "SELECT dolt_commit('-am', 'x');",
                "SELECT dolt_checkout('y');",
                "ALTER TABLE t ADD COLUMN c TEXT;",
                "SELECT dolt_commit('-am', 'y');",
                "SELECT dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("incompatible column types for column 'c': integer and text")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts_t;",
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
fn test_type_changes_rebuild_what_depends_on_the_column() {
    run_scripts(&[
        ScriptTest {
            name: "a merge taking theirs' type change rebuilds ours' foreign key onto the column",
            intent: "Theirs widens the referenced column to bigint while ours adds a foreign key onto it, so the merged foreign key compares with the operators Postgres gives a bigint key and an integer column, and a missing key is reported as missing.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT UNIQUE);",
                "INSERT INTO parent VALUES (1, 10), (2, 20);",
                "CREATE TABLE child (id INT PRIMARY KEY, x INT);",
                "INSERT INTO child VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE parent ALTER COLUMN p TYPE BIGINT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE child ADD CONSTRAINT child_x_fkey FOREIGN KEY (x) REFERENCES parent (p);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SELECT dolt_merge('x', '-m', 'merge x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT conpfeqop::regoperator[]::text, conppeqop::regoperator[]::text, conffeqop::regoperator[]::text FROM pg_constraint WHERE conname = 'child_x_fkey';",
                    expected: Expected::Rows {
                        columns: &[Column("conpfeqop", TEXT), Column("conppeqop", TEXT), Column("conffeqop", TEXT)],
                        rows: &[
                            &[T("{\"=(bigint,integer)\"}"), T("{\"=(bigint,bigint)\"}"), T("{\"=(integer,integer)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO parent VALUES (5, 4294967336);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (4, 40);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"child\" violates foreign key constraint \"child_x_fkey\"", detail: "Key (x)=(40) is not present in table \"parent\".", schema: "public", table: "child", constraint: "child_x_fkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge taking theirs' type change rebuilds ours' index on the column",
            intent: "Theirs changes the indexed column to text while ours adds the index, so the merged index takes text's default operator class, as Postgres builds it again after ALTER COLUMN TYPE, and finds rows by it.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT);",
                "INSERT INTO parent VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE parent ALTER COLUMN p TYPE TEXT;",
                "UPDATE parent SET p = 'b' || p;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "CREATE INDEX parent_p ON parent (p);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SELECT dolt_merge('x', '-m', 'merge x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT opcname FROM pg_opclass WHERE oid = (SELECT indclass[0] FROM pg_index WHERE indexrelid = 'parent_p'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("opcname", NAME)],
                        rows: &[
                            &[T("text_ops")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT atttypid::regtype::text, attcollation::regcollation::text FROM pg_attribute WHERE attrelid = 'parent_p'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("atttypid", TEXT), Column("attcollation", TEXT)],
                        rows: &[
                            &[T("text"), T("\"default\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k FROM parent WHERE p > 'b15';",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4)],
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
            name: "a merge taking theirs' type change rebuilds ours' exclusion constraint on the column",
            intent: "Theirs widens the column to bigint while ours adds an exclusion constraint on it, so the merged constraint compares bigints and refuses a second row of one large key.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT);",
                "INSERT INTO parent VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE parent ALTER COLUMN p TYPE BIGINT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE parent ADD CONSTRAINT ex EXCLUDE USING btree (p WITH =);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SELECT dolt_merge('x', '-m', 'merge x');",
                "INSERT INTO parent VALUES (3, 4294967306);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO parent VALUES (4, 4294967306);",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: "conflicting key value violates exclusion constraint \"ex\"", detail: "Key (p)=(4294967306) conflicts with existing key (p)=(4294967306).", schema: "public", table: "parent", constraint: "ex", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checking out a table rebuilds the foreign keys onto its columns",
            intent: "Checking out the referenced table gives its column back the integer type its staged root has, so the working state's foreign key onto it takes the operators Postgres gives an integer key and a bigint column.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT UNIQUE);",
                "INSERT INTO parent VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'c1');",
                "ALTER TABLE parent ALTER COLUMN p TYPE BIGINT;",
                "CREATE TABLE child (id INT PRIMARY KEY, x BIGINT REFERENCES parent (p));",
                "INSERT INTO child VALUES (1, 10);",
                "SELECT dolt_checkout('parent');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT conpfeqop::regoperator[]::text, conppeqop::regoperator[]::text, conffeqop::regoperator[]::text FROM pg_constraint WHERE conname = 'child_x_fkey';",
                    expected: Expected::Rows {
                        columns: &[Column("conpfeqop", TEXT), Column("conppeqop", TEXT), Column("conffeqop", TEXT)],
                        rows: &[
                            &[T("{\"=(integer,bigint)\"}"), T("{\"=(integer,integer)\"}"), T("{\"=(bigint,bigint)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checking out a table refuses while rows no longer find the keys it retypes",
            intent: "Checking out the referenced table gives its key back the rows and the numeric(3,0) type of its staged root, so the working state's referencing row finds no key and it refuses as Postgres' ALTER COLUMN TYPE does when it checks the rebuilt foreign key.",
            set_up_script: &[
                "CREATE TABLE p4 (k INT PRIMARY KEY, d NUMERIC(3, 0) UNIQUE);",
                "SELECT dolt_commit('-Am', 'c1');",
                "ALTER TABLE p4 ALTER COLUMN d TYPE NUMERIC;",
                "INSERT INTO p4 VALUES (1, 1.5);",
                "CREATE TABLE c4 (id INT PRIMARY KEY, d NUMERIC REFERENCES p4 (d));",
                "INSERT INTO c4 VALUES (1, 1.5);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('p4');",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"c4\" violates foreign key constraint \"c4_d_fkey\"", detail: "Key (d)=(1.5) is not present in table \"p4\".", schema: "public", table: "c4", constraint: "c4_d_fkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge refuses theirs' type change while ours' index names an operator class the new type lacks",
            intent: "Ours indexes the text column with text_pattern_ops and theirs changes it to name, which that operator class does not accept, so the merge fails as Postgres' ALTER COLUMN TYPE fails building the index again.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p TEXT);",
                "INSERT INTO parent VALUES (1, 'a'), (2, 'b');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE parent ALTER COLUMN p TYPE NAME;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "CREATE INDEX parent_p ON parent (p text_pattern_ops);",
                "SELECT dolt_commit('-Am', 'm1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "operator class \"text_pattern_ops\" does not accept data type name", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge taking theirs' type change gives ours' index the column's new collation",
            intent: "Ours indexes a column whose collation is C without naming a collation, and theirs changes it to varchar, whose collation is the default, so the merged index takes the column's new collation as Postgres builds it again after ALTER COLUMN TYPE.",
            set_up_script: &[
                "CREATE TABLE rt (k INT PRIMARY KEY, c TEXT COLLATE \"C\");",
                "INSERT INTO rt VALUES (1, 'a');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE rt ALTER COLUMN c TYPE VARCHAR(20);",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "CREATE INDEX rt_c ON rt (c);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SELECT dolt_merge('x', '-m', 'merge x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT indcollation[0]::regcollation::text, (SELECT attcollation::regcollation::text FROM pg_attribute WHERE attrelid = 'rt_c'::regclass) FROM pg_index WHERE indexrelid = 'rt_c'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indcollation", TEXT), Column("attcollation", TEXT)],
                        rows: &[
                            &[T("\"default\""), T("\"default\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge taking theirs' type change rebuilds ours' index with an included column",
            intent: "Ours indexes the column with another column included and theirs widens the indexed column to bigint, so the merged index takes bigint's operator class for its one key column and keeps its included column, as Postgres builds it again after ALTER COLUMN TYPE.",
            set_up_script: &[
                "CREATE TABLE ic (k INT PRIMARY KEY, p INT, q TEXT);",
                "INSERT INTO ic VALUES (1, 10, 'a');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE ic ALTER COLUMN p TYPE BIGINT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "CREATE INDEX ic_p ON ic (p) INCLUDE (q);",
                "SELECT dolt_commit('-Am', 'm1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('x', '-m', 'merge x') IS NOT NULL;",
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
                    query: "SELECT cardinality(indclass::oid[]), (SELECT opcname FROM pg_opclass WHERE oid = indclass[0]) FROM pg_index WHERE indexrelid = 'ic_p'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("cardinality", INT4), Column("opcname", NAME)],
                        rows: &[
                            &[T("1"), T("int8_ops")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, atttypid::regtype::text FROM pg_attribute WHERE attrelid = 'ic_p'::regclass ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("atttypid", TEXT)],
                        rows: &[
                            &[T("p"), T("bigint")],
                            &[T("q"), T("text")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge refuses theirs' type change while ours' index names a collation the new type lacks",
            intent: "Ours indexes the text column with an explicit C collation and theirs changes it to \"char\", which has no collations, so the merge fails as Postgres' ALTER COLUMN TYPE fails building the index again.",
            set_up_script: &[
                "CREATE TABLE rt_x (k INT PRIMARY KEY, d TEXT);",
                "INSERT INTO rt_x VALUES (1, 'a');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE rt_x ALTER COLUMN d TYPE \"char\";",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "CREATE INDEX rt_x_d ON rt_x (d COLLATE \"C\");",
                "SELECT dolt_commit('-Am', 'm1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "collations are not supported by type \"char\"", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge taking theirs' type change drops the collation ours' index took from the column",
            intent: "Ours indexes a text column whose collation is C without naming a collation, and theirs changes it to \"char\", which has no collations, so the merged index has none, as Postgres builds it again after ALTER COLUMN TYPE.",
            set_up_script: &[
                "CREATE TABLE rt_y (k INT PRIMARY KEY, d TEXT COLLATE \"C\");",
                "INSERT INTO rt_y VALUES (1, 'a');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE rt_y ALTER COLUMN d TYPE \"char\";",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "CREATE INDEX rt_y_d ON rt_y (d);",
                "SELECT dolt_commit('-Am', 'm1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('x', '-m', 'merge x') IS NOT NULL;",
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
                    query: "SELECT indcollation[0]::int FROM pg_index WHERE indexrelid = 'rt_y_d'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indcollation", INT4)],
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
            name: "a merge taking theirs' type change rebuilds ours' checks, index expressions and predicates, and defaults on the column",
            intent: "Theirs widens the column to bigint while ours adds a CHECK constraint, an expression index, a partial index, and a default over it, so the merged objects work on bigint values as Postgres builds them again after ALTER COLUMN TYPE.",
            set_up_script: &[
                "CREATE TABLE ce (k INT PRIMARY KEY, p INT);",
                "INSERT INTO ce VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE ce ALTER COLUMN p TYPE BIGINT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE ce ADD CONSTRAINT ce_pos CHECK (p > 5);",
                "CREATE INDEX ce_e ON ce ((p + 1));",
                "CREATE INDEX ce_w ON ce (k) WHERE p > 15;",
                "ALTER TABLE ce ALTER COLUMN p SET DEFAULT 7;",
                "SELECT dolt_commit('-Am', 'm1');",
                "SELECT dolt_merge('x', '-m', 'merge x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'ce_pos';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("CHECK ((p > 5))")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_indexdef('ce_e'::regclass), pg_get_indexdef('ce_w'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_indexdef", TEXT), Column("pg_get_indexdef", TEXT)],
                        rows: &[
                            &[T("CREATE INDEX ce_e ON public.ce USING btree (((p + 1)))"), T("CREATE INDEX ce_w ON public.ce USING btree (k) WHERE (p > 15)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_expr(adbin, adrelid) FROM pg_attrdef WHERE adrelid = 'ce'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_expr", TEXT)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ce VALUES (3, 5000000000);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ce VALUES (5, 3);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: "new row for relation \"ce\" violates check constraint \"ce_pos\"", detail: "Failing row contains (5, 3).", schema: "public", table: "ce", constraint: "ce_pos", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ce (k) VALUES (6) RETURNING p;",
                    expected: Expected::Rows {
                        columns: &[Column("p", INT8)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k FROM ce WHERE p + 1 = 5000000001;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge taking theirs' type change removes the column's statistics and the data of statistics objects on it",
            intent: "Ours analyzed the integer column and theirs widens it to bigint, so the merge leaves no statistics for the column and no data for the statistics object on it until ANALYZE runs again, as Postgres' ALTER COLUMN TYPE removes them.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT);",
                "INSERT INTO parent SELECT g, g % 5 FROM generate_series(1, 100) g;",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE parent ALTER COLUMN p TYPE BIGINT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "CREATE STATISTICS s1 (ndistinct) ON p, k FROM parent;",
                "ANALYZE parent;",
                "SELECT dolt_commit('-Am', 'm1');",
                "SELECT dolt_merge('x', '-m', 'merge x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT attname, n_distinct FROM pg_stats WHERE tablename = 'parent' ORDER BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("n_distinct", FLOAT4)],
                        rows: &[
                            &[T("k"), T("-1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stats_ext WHERE statistics_name = 's1' AND n_distinct IS NOT NULL;",
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
                    query: "ANALYZE parent;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, n_distinct FROM pg_stats WHERE tablename = 'parent' ORDER BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("n_distinct", FLOAT4)],
                        rows: &[
                            &[T("k"), T("-1")],
                            &[T("p"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stats_ext WHERE statistics_name = 's1' AND n_distinct IS NOT NULL;",
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
            name: "a hard reset that gives a column back its type removes its statistics and the data of statistics objects on it",
            intent: "The working state widened the column to bigint and analyzed it, and a hard reset gives it back its committed integer type, so it leaves no statistics for the column and no data for the statistics object on it until ANALYZE runs again, as Postgres' ALTER COLUMN TYPE removes them.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT);",
                "INSERT INTO parent SELECT g, g % 5 FROM generate_series(1, 100) g;",
                "CREATE STATISTICS s1 (ndistinct) ON p, k FROM parent;",
                "SELECT dolt_commit('-Am', 'c1');",
                "ALTER TABLE parent ALTER COLUMN p TYPE BIGINT;",
                "ANALYZE parent;",
                "SELECT dolt_reset('--hard');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT attname, n_distinct FROM pg_stats WHERE tablename = 'parent' ORDER BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("n_distinct", FLOAT4)],
                        rows: &[
                            &[T("k"), T("-1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stats_ext WHERE statistics_name = 's1' AND n_distinct IS NOT NULL;",
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
                    query: "ANALYZE parent;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, n_distinct FROM pg_stats WHERE tablename = 'parent' ORDER BY attname;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("n_distinct", FLOAT4)],
                        rows: &[
                            &[T("k"), T("-1")],
                            &[T("p"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stats_ext WHERE statistics_name = 's1' AND n_distinct IS NOT NULL;",
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
            name: "a merge taking theirs' type change rebuilds ours' statistics expression on the column",
            intent: "Theirs widens the column to bigint while ours adds statistics on an expression over it, so the merged statistics object computes the expression over bigint values when ANALYZE runs, as Postgres builds it again after ALTER COLUMN TYPE.",
            set_up_script: &[
                "CREATE TABLE sz (k INT PRIMARY KEY, p INT);",
                "INSERT INTO sz SELECT g, g % 5 FROM generate_series(1, 100) g;",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE sz ALTER COLUMN p TYPE BIGINT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "CREATE STATISTICS sz_s1 ON (p + 1), k FROM sz;",
                "SELECT dolt_commit('-Am', 'm1');",
                "SELECT dolt_merge('x', '-m', 'merge x');",
                "ANALYZE sz;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT expr, n_distinct, most_common_vals::text FROM pg_stats_ext_exprs WHERE statistics_name = 'sz_s1';",
                    expected: Expected::Rows {
                        columns: &[Column("expr", TEXT), Column("n_distinct", FLOAT4), Column("most_common_vals", TEXT)],
                        rows: &[
                            &[T("(p + 1)"), T("5"), T("{1,2,3,4,5}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge refuses theirs' type change while ours' statistics expression can't take the new type",
            intent: "Theirs changes the column to text while ours adds statistics on p + 1, which has no operator for text, so the merge fails as Postgres' ALTER COLUMN TYPE fails building the statistics object again.",
            set_up_script: &[
                "CREATE TABLE sx (k INT PRIMARY KEY, p INT);",
                "INSERT INTO sx VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE sx ALTER COLUMN p TYPE TEXT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "CREATE STATISTICS sx_s1 ON (p + 1), k FROM sx;",
                "SELECT dolt_commit('-Am', 'm1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: text + integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 41, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge taking theirs' type change conflicts while ours' view depends on the column",
            intent: "Theirs widens the column to bigint while ours adds a view over it, which Postgres' ALTER COLUMN TYPE refuses, so the table's definitions conflict, resolving with --theirs refuses as that ALTER does, and resolving with --ours keeps the view.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT);",
                "INSERT INTO parent VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE parent ALTER COLUMN p TYPE BIGINT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "CREATE VIEW v AS SELECT p + 1 AS p1 FROM parent;",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.parent"), T("cannot alter type of a column used by a view or rule: rule _RETURN on view v depends on column \"p\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--theirs', 'parent');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot alter type of a column used by a view or rule", detail: "rule _RETURN on view v depends on column \"p\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_conflicts_resolve('--ours', 'parent');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p1 FROM v ORDER BY p1;",
                    expected: Expected::Rows {
                        columns: &[Column("p1", INT4)],
                        rows: &[
                            &[T("11")],
                            &[T("21")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge taking theirs' view conflicts while ours changed the type of the column it depends on",
            intent: "Ours widens the column to bigint while theirs adds a view over it, which Postgres' ALTER COLUMN TYPE refuses, so the table's definitions conflict and the merge holds the view back with it.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT);",
                "INSERT INTO parent VALUES (1, 10), (2, 20);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "CREATE VIEW v AS SELECT p + 1 AS p1 FROM parent;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE parent ALTER COLUMN p TYPE BIGINT;",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.parent"), T("cannot alter type of a column used by a view or rule: rule _RETURN on view v depends on column \"p\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname = 'v';",
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
            name: "a merge taking theirs' type changes conflicts while ours' generated column or policy depends on the column",
            intent: "Theirs widens two tables' columns to bigint while ours adds a generated column over one and a policy over the other, which Postgres' ALTER COLUMN TYPE refuses, so both tables' definitions conflict.",
            set_up_script: &[
                "CREATE TABLE gq (k INT PRIMARY KEY, p INT);",
                "INSERT INTO gq VALUES (1, 10);",
                "CREATE TABLE pq (k INT PRIMARY KEY, p INT);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE gq ALTER COLUMN p TYPE BIGINT;",
                "ALTER TABLE pq ALTER COLUMN p TYPE BIGINT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE gq ADD COLUMN g BIGINT GENERATED ALWAYS AS (p * 2) STORED;",
                "CREATE POLICY pq_pol ON pq USING (p > 0);",
                "SELECT dolt_commit('-Am', 'm1');",
                "SET dolt_allow_commit_conflicts = on;",
                "SELECT dolt_merge('x', '-m', 'merge x');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT table_name, description FROM dolt_schema_conflicts ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.gq"), T("cannot alter type of a column used by a generated column: Column \"p\" is used by generated column \"g\".")],
                            &[T("public.pq"), T("cannot alter type of a column used in a policy definition: policy pq_pol on table pq depends on column \"p\"")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checking out a table refuses while a view depends on a column it retypes",
            intent: "Checking out the table gives its column back the integer type its staged root has while the working state's view depends on the column, which Postgres' ALTER COLUMN TYPE refuses, so the checkout refuses as that ALTER does.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT);",
                "INSERT INTO parent VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'c1');",
                "ALTER TABLE parent ALTER COLUMN p TYPE BIGINT;",
                "CREATE VIEW v AS SELECT p + 1 AS p1 FROM parent;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('parent');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot alter type of a column used by a view or rule", detail: "rule _RETURN on view v depends on column \"p\"", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge taking theirs' dropped and added column of one name leaves no statistics for either",
            intent: "Theirs drops the integer column b and adds a text column b, and ours analyzed the integer one, so the merge leaves no statistics for the dropped column nor any for the new one until ANALYZE runs again, as Postgres' DROP COLUMN removes them.",
            set_up_script: &[
                "CREATE TABLE t (k INT PRIMARY KEY, b INT);",
                "INSERT INTO t SELECT g, g % 7 FROM generate_series(1, 100) g;",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t DROP COLUMN b;",
                "ALTER TABLE t ADD COLUMN b TEXT;",
                "UPDATE t SET b = 'v' || k;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ANALYZE t;",
                "SELECT dolt_merge('x', '-m', 'merge x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT staattnum, stadistinct FROM pg_statistic WHERE starelid = 't'::regclass ORDER BY staattnum;",
                    expected: Expected::Rows {
                        columns: &[Column("staattnum", INT2), Column("stadistinct", FLOAT4)],
                        rows: &[
                            &[T("1"), T("-1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "checking out a table whose column of one name was dropped and added leaves no statistics for either",
            intent: "The other branch's table drops the integer column b and adds a text column b, and the working state analyzed the integer one, so checking out the table from that branch leaves no statistics for either column until ANALYZE runs again.",
            set_up_script: &[
                "CREATE TABLE t (k INT PRIMARY KEY, b INT);",
                "INSERT INTO t SELECT g, g % 7 FROM generate_series(1, 100) g;",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t DROP COLUMN b;",
                "ALTER TABLE t ADD COLUMN b TEXT;",
                "UPDATE t SET b = 'v' || k;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ANALYZE t;",
                "SELECT dolt_checkout('x', '--', 't');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT staattnum, stadistinct FROM pg_statistic WHERE starelid = 't'::regclass ORDER BY staattnum;",
                    expected: Expected::Rows {
                        columns: &[Column("staattnum", INT2), Column("stadistinct", FLOAT4)],
                        rows: &[
                            &[T("1"), T("-1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a hard reset that gives columns other numbers leaves no statistics at a number whose column changed",
            intent: "The working state dropped a and added it again at the end, and analyzed the table, so a hard reset gives a its number back before b, and only b, whose number and column stay, keeps its statistics.",
            set_up_script: &[
                "CREATE TABLE t (k INT PRIMARY KEY, a INT, b INT);",
                "INSERT INTO t SELECT g, g % 3, g % 7 FROM generate_series(1, 100) g;",
                "SELECT dolt_commit('-Am', 'c1');",
                "ALTER TABLE t DROP COLUMN a;",
                "ALTER TABLE t ADD COLUMN a INT;",
                "UPDATE t SET a = k % 11;",
                "ANALYZE t;",
                "SELECT dolt_reset('--hard');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT attname, attnum FROM pg_attribute WHERE attrelid = 't'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnum", INT2)],
                        rows: &[
                            &[T("k"), T("1")],
                            &[T("a"), T("2")],
                            &[T("b"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT staattnum, stadistinct FROM pg_statistic WHERE starelid = 't'::regclass ORDER BY staattnum;",
                    expected: Expected::Rows {
                        columns: &[Column("staattnum", INT2), Column("stadistinct", FLOAT4)],
                        rows: &[
                            &[T("1"), T("-1")],
                            &[T("3"), T("7")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a hard reset that drops a column removes its statistics",
            intent: "The working state added and analyzed column c, so a hard reset that drops it removes its statistics, as Postgres' DROP COLUMN does.",
            set_up_script: &[
                "CREATE TABLE t (k INT PRIMARY KEY, b INT);",
                "INSERT INTO t SELECT g, g % 7 FROM generate_series(1, 100) g;",
                "SELECT dolt_commit('-Am', 'c1');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "UPDATE t SET c = k % 5;",
                "ANALYZE t;",
                "SELECT dolt_reset('--hard');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT staattnum, stadistinct FROM pg_statistic WHERE starelid = 't'::regclass ORDER BY staattnum;",
                    expected: Expected::Rows {
                        columns: &[Column("staattnum", INT2), Column("stadistinct", FLOAT4)],
                        rows: &[
                            &[T("1"), T("-1")],
                            &[T("2"), T("7")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge refuses theirs' type change while ours' index expression can't take the new type",
            intent: "Theirs changes a to text while ours indexes a + b, which has no operator for text, so the merge fails where Postgres' ALTER COLUMN TYPE fails building the index again, pointing into the index's definition.",
            set_up_script: &[
                "CREATE TABLE it (k INT PRIMARY KEY, a INT, b INT);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE it ALTER COLUMN a TYPE TEXT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "CREATE INDEX ON it ((a + b));",
                "SELECT dolt_commit('-Am', 'm1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE in alter_table::test_alter_type_errors_point_into_the_definitions_it_creates_again.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: text + integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 56, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge refuses theirs' type change while ours' exclusion constraint expression can't take the new type",
            intent: "Theirs changes a to text while ours adds an exclusion constraint on a + b, which has no operator for text, so the merge fails where Postgres' ALTER COLUMN TYPE fails building the constraint again, pointing into the constraint's definition.",
            set_up_script: &[
                "CREATE TABLE ex (k INT PRIMARY KEY, a INT, b INT);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE ex ALTER COLUMN a TYPE TEXT;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_checkout('main');",
                "ALTER TABLE ex ADD EXCLUDE USING btree ((a + b) WITH =);",
                "SELECT dolt_commit('-Am', 'm1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE in alter_table::test_alter_type_errors_point_into_the_definitions_it_creates_again.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('x', '-m', 'merge x');",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: text + integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 76, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_staging_keeps_what_depends_on_a_retyped_column_consistent() {
    run_scripts(&[
        ScriptTest {
            name: "staging a foreign key alone rebuilds it for the staged root's type of the column it references",
            intent: "The working state widens parent.p to bigint and adds child's foreign key onto it, and staging child alone leaves the staged root's p an integer, so the committed foreign key takes the operators Postgres gives an integer key and a bigint column.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT UNIQUE);",
                "INSERT INTO parent VALUES (1, 10);",
                "SELECT dolt_commit('-Am', 'c1');",
                "ALTER TABLE parent ALTER COLUMN p TYPE BIGINT;",
                "CREATE TABLE child (id INT PRIMARY KEY, x BIGINT REFERENCES parent (p));",
                "SELECT dolt_add('child');",
                "SELECT dolt_commit('-m', 'child');",
                "SELECT dolt_branch('b2', 'HEAD');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status ORDER BY table_name, staged;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.child"), T("f"), T("modified")],
                            &[T("public.parent"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('b2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T("(0,\"Switched to branch 'b2'\")")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conpfeqop::regoperator[]::text, conppeqop::regoperator[]::text, conffeqop::regoperator[]::text FROM pg_constraint WHERE conname = 'child_x_fkey';",
                    expected: Expected::Rows {
                        columns: &[Column("conpfeqop", TEXT), Column("conppeqop", TEXT), Column("conffeqop", TEXT)],
                        rows: &[
                            &[T("{\"=(integer,bigint)\"}"), T("{\"=(integer,integer)\"}"), T("{\"=(bigint,bigint)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "staging a type change rebuilds what the staged root has on the column and stages a dropped view with it",
            intent: "The working state drops a view over parent.p and widens p to bigint, which rebuilds child's foreign key and parent's index, and staging parent alone takes the view's deletion along and gives the staged foreign key and index the forms Postgres gives them for bigint.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT UNIQUE);",
                "INSERT INTO parent VALUES (1, 10);",
                "CREATE TABLE child (id INT PRIMARY KEY, x INT REFERENCES parent (p));",
                "CREATE INDEX parent_pi ON parent (p);",
                "CREATE VIEW v AS SELECT p + 1 AS p1 FROM parent;",
                "CREATE VIEW w AS SELECT k FROM parent;",
                "SELECT dolt_commit('-Am', 'c1');",
                "DROP VIEW v;",
                "ALTER TABLE parent ALTER COLUMN p TYPE BIGINT;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('parent');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status ORDER BY table_name, staged;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.child"), T("t"), T("modified")],
                            &[T("public.parent"), T("t"), T("modified")],
                            &[T("view public.v"), T("t"), T("deleted")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'parent') IS NOT NULL;",
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
                    query: "SELECT dolt_branch('b2', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('b2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T("(0,\"Switched to branch 'b2'\")")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conpfeqop::regoperator[]::text, conppeqop::regoperator[]::text, conffeqop::regoperator[]::text FROM pg_constraint WHERE conname = 'child_x_fkey';",
                    expected: Expected::Rows {
                        columns: &[Column("conpfeqop", TEXT), Column("conppeqop", TEXT), Column("conffeqop", TEXT)],
                        rows: &[
                            &[T("{\"=(bigint,integer)\"}"), T("{\"=(bigint,bigint)\"}"), T("{\"=(integer,integer)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT opcname FROM pg_opclass WHERE oid = (SELECT indclass[0] FROM pg_index WHERE indexrelid = 'parent_pi'::regclass);",
                    expected: Expected::Rows {
                        columns: &[Column("opcname", NAME)],
                        rows: &[
                            &[T("int8_ops")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname FROM pg_class WHERE relname IN ('v', 'w') ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME)],
                        rows: &[
                            &[T("w")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "staging a view alone stages the type change of the column it needs",
            intent: "The working state drops a view, widens parent.p to bigint, and adds another view over p, so staging the new view alone takes parent's type change along, which takes the dropped view's deletion with it, since neither view could stand over the staged root's integer p.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT);",
                "INSERT INTO parent VALUES (1, 10);",
                "CREATE VIEW v AS SELECT p + 1 AS p1 FROM parent;",
                "SELECT dolt_commit('-Am', 'c1');",
                "DROP VIEW v;",
                "ALTER TABLE parent ALTER COLUMN p TYPE BIGINT;",
                "CREATE VIEW v2 AS SELECT p * 2 AS p2 FROM parent;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('v2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "dolt_add also stages public.parent", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status ORDER BY table_name, staged;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.parent"), T("t"), T("modified")],
                            &[T("view public.v"), T("t"), T("deleted")],
                            &[T("view public.v2"), T("t"), T("new view")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "committing all tracked changes keeps the foreign keys on a retyped column consistent",
            intent: "The working state widens parent.p to bigint, which rebuilds child's foreign key onto it, and committing with -a takes both tables, so the committed foreign key has the operators Postgres gives a bigint key and an integer column.",
            set_up_script: &[
                "CREATE TABLE parent (k INT PRIMARY KEY, p INT UNIQUE);",
                "INSERT INTO parent VALUES (1, 10);",
                "CREATE TABLE child (id INT PRIMARY KEY, x INT REFERENCES parent (p));",
                "SELECT dolt_commit('-Am', 'c1');",
                "ALTER TABLE parent ALTER COLUMN p TYPE BIGINT;",
                "SELECT dolt_commit('-a', '-m', 'retype');",
                "SELECT dolt_branch('b2', 'HEAD');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected, checked against PG18's ALTER COLUMN TYPE.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('b2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T("(0,\"Switched to branch 'b2'\")")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conpfeqop::regoperator[]::text, conppeqop::regoperator[]::text, conffeqop::regoperator[]::text FROM pg_constraint WHERE conname = 'child_x_fkey';",
                    expected: Expected::Rows {
                        columns: &[Column("conpfeqop", TEXT), Column("conppeqop", TEXT), Column("conffeqop", TEXT)],
                        rows: &[
                            &[T("{\"=(bigint,integer)\"}"), T("{\"=(bigint,bigint)\"}"), T("{\"=(integer,integer)\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "staging a composite type's attribute type change stages the table that used it",
            intent: "The working state drops t's column of the composite type and widens one of the type's attributes, so staging the type alone takes t's change along, since the staged t's column could not stand over the changed type.",
            set_up_script: &[
                "CREATE TYPE comp AS (a INT, b INT);",
                "CREATE TABLE t (k INT PRIMARY KEY, c comp);",
                "INSERT INTO t VALUES (1, ROW(1, 2));",
                "SELECT dolt_commit('-Am', 'c1');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TYPE comp ALTER ATTRIBUTE a TYPE BIGINT;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('comp');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "dolt_add also stages public.t", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status ORDER BY table_name, staged;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("composite type public.comp"), T("t"), T("modified")],
                            &[T("public.t"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "staging a table whose column uses a composite type stages the type's attribute type change",
            intent: "The working state widens one of the composite type's attributes and adds a table with a column of that type, so staging the table alone takes the type's change along, since its column needs the type's working form.",
            set_up_script: &[
                "CREATE TYPE comp AS (a INT, b INT);",
                "SELECT dolt_commit('-Am', 'c1');",
                "ALTER TYPE comp ALTER ATTRIBUTE a TYPE BIGINT;",
                "CREATE TABLE t2 (k INT PRIMARY KEY, c comp);",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { code: "00000", message: "dolt_add also stages composite type public.comp", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, staged, status FROM dolt_status ORDER BY table_name, staged;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("composite type public.comp"), T("t"), T("modified")],
                            &[T("public.t2"), T("t"), T("new table")],
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
fn test_dolt_count_commits() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_count_commits counts to the tallest merge base and takes no other arguments",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('b2');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'x1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'x2');",
                "SELECT dolt_tag('x2');",
                "SELECT dolt_checkout('b2');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'y1');",
                "SELECT dolt_tag('y1');",
                "SELECT dolt_checkout('main');",
                "SELECT dolt_merge('y1', '--no-ff', '-m', 'm1');",
                "SELECT dolt_checkout('b2');",
                "INSERT INTO t VALUES (11);",
                "SELECT dolt_commit('-am', 'y2');",
                "SELECT dolt_merge('x2', '--no-ff', '-m', 'm2');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_count_commits('--from', 'main', '--to', 'b2');",
                    expected: Expected::Rows {
                        columns: &[Column("ahead", INT8), Column("behind", INT8)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_count_commits('--from', 'b2', '--to', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("ahead", INT8), Column("behind", INT8)],
                        rows: &[
                            &[T("2"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_count_commits('-f', 'main', '-t', 'x2');",
                    expected: Expected::Rows {
                        columns: &[Column("ahead", INT8), Column("behind", INT8)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, and Dolt's message names `gc`, a Dolt bug.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_count_commits('--from', 'main', '--to', 'b2', 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: dolt_count_commits does not take positional arguments, but found 1: x", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_count_commits('--from', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "missing to ref", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_history_readers_check_the_tables_they_show() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_diff, dolt_column_diff, and dolt_diff_summary need SELECT on a table only history still has",
            intent: "A table dropped from the branch is still shown by the history system tables, so a role without SELECT on it as history grants it is refused, as it is for the branch's own tables.",
            set_up_script: &[
                "CREATE USER tester PASSWORD 'tester';",
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "GRANT SELECT ON t TO tester;",
                "CREATE TABLE secret (pk int PRIMARY KEY, v text);",
                "INSERT INTO secret VALUES (1, 'hidden');",
                "SELECT dolt_commit('-Am', 'add secret');",
                "DROP TABLE secret;",
                "SELECT dolt_commit('-Am', 'drop secret');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    username: "tester",
                    password: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_diff;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table secret", ..E }),
                    username: "tester",
                    password: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, column_name FROM dolt_column_diff;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table secret", ..E }),
                    username: "tester",
                    password: "tester",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_table_name FROM dolt_diff_summary('HEAD~1', 'HEAD');",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table secret", ..E }),
                    username: "tester",
                    password: "tester",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_merge_base() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_merge_base returns the tallest merge base and dolt_merge_bases every one, newest first",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('b2');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'x1');",
                "INSERT INTO t VALUES (2);",
                "SELECT dolt_commit('-am', 'x2');",
                "SELECT dolt_tag('x2');",
                "SELECT dolt_checkout('b2');",
                "INSERT INTO t VALUES (10);",
                "SELECT dolt_commit('-am', 'y1');",
                "SELECT dolt_tag('y1');",
                "SELECT dolt_checkout('main');",
                "SELECT dolt_merge('y1', '--no-ff', '-m', 'm1');",
                "SELECT dolt_checkout('b2');",
                "INSERT INTO t VALUES (11);",
                "SELECT dolt_commit('-am', 'y2');",
                "SELECT dolt_merge('x2', '--no-ff', '-m', 'm2');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge_base('main', 'b2') = dolt_hashof('x2') AS tallest;",
                    expected: Expected::Rows {
                        columns: &[Column("tallest", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT tg.tag_name FROM (SELECT merge_base, ROW_NUMBER() OVER () AS rn FROM dolt_merge_bases('main', 'b2')) b JOIN dolt_tags tg ON tg.tag_hash = b.merge_base ORDER BY b.rn;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("y1")],
                            &[T("x2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge_base('main', 'b2~1') = dolt_hashof('y1') AS parent;",
                    expected: Expected::Rows {
                        columns: &[Column("parent", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge_base(NULL, 'main') IS NULL AS unknown;",
                    expected: Expected::Rows {
                        columns: &[Column("unknown", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge_base('main', 'nope');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: nope", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge_base('', 'main');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "string is not a valid branch or hash", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_gc_with_shallow_and_full() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_gc refuses --shallow together with --full as Dolt does",
            set_up_script: &[],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_gc('--shallow', '--full');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot supply both --shallow and --full to dolt_gc: error: invalid usage", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
