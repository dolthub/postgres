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
fn test_locking_clauses() {
    run_scripts(&[
        ScriptTest {
            name: "locking clauses are rejected by default",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 1 FOR UPDATE",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "unsupported locking clauses are permitted",
            server_config: r#"behavior:
  permit_unsupported_locking_statements: true
"#,
            set_up_script: &[
                "CREATE TABLE locking_test (pk INT PRIMARY KEY)",
                "INSERT INTO locking_test VALUES (1), (2)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM locking_test ORDER BY pk FOR UPDATE",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM locking_test ORDER BY pk FOR NO KEY UPDATE NOWAIT",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM locking_test ORDER BY pk FOR SHARE SKIP LOCKED",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM locking_test ORDER BY pk FOR KEY SHARE OF locking_test",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
fn test_row_locks_of_concurrent_updates() {
    run_scripts(&[
        ScriptTest {
            name: "ON CONFLICT DO UPDATE on a table without a primary key updates the row a concurrent insert committed",
            set_up_script: &[
                "CREATE TABLE ku (key text NOT NULL, payload text)",
                "CREATE UNIQUE INDEX ON ku (key)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO ku VALUES ('k', 'A') ON CONFLICT (key) DO UPDATE SET payload = ku.payload || ' updated by A'",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO ku VALUES ('k', 'B') ON CONFLICT (key) DO UPDATE SET payload = ku.payload || ' updated by B'",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT key, payload FROM ku",
                    expected: Expected::Rows {
                        columns: &[Column("key", TEXT), Column("payload", TEXT)],
                        rows: &[
                            &[T("k"), T("A updated by B")],
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
            name: "ON CONFLICT DO UPDATE in REPEATABLE READ fails on a row a concurrent insert committed to a table without a primary key",
            set_up_script: &[
                "CREATE TABLE ku (key text NOT NULL, payload text)",
                "CREATE UNIQUE INDEX ON ku (key)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO ku VALUES ('k', 'A') ON CONFLICT (key) DO UPDATE SET payload = ku.payload || ' updated by A'",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO ku VALUES ('k', 'B') ON CONFLICT (key) DO UPDATE SET payload = ku.payload || ' updated by B'",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT key, payload FROM ku",
                    expected: Expected::Rows {
                        columns: &[Column("key", TEXT), Column("payload", TEXT)],
                        rows: &[
                            &[T("k"), T("A")],
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
            name: "an update of a table without a primary key that changes no unique column passes FOR KEY SHARE locks",
            set_up_script: &[
                "CREATE TABLE kp (i int, c text)",
                "CREATE UNIQUE INDEX ON kp (i)",
                "INSERT INTO kp VALUES (1, 'AAA')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT i, c FROM kp FOR KEY SHARE",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("1"), T("AAA")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE kp SET c = lower(c)",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE kp SET i = i",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE kp SET i = 2",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT i, c FROM kp",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("c", TEXT)],
                        rows: &[
                            &[T("2"), T("aaa")],
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
            name: "a FOR KEY SHARE lock takes the row its snapshot saw over a committed update that changed no key",
            set_up_script: &[
                "CREATE TABLE lcu (id int PRIMARY KEY, value text)",
                "INSERT INTO lcu VALUES (1, 'one')",
                "CREATE TABLE seen (value text)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT id, value FROM lcu",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("value", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE lcu SET value = 'two' WHERE id = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT id, value FROM lcu FOR KEY SHARE",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("value", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT id, value FROM lcu FOR SHARE",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_lock(7)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_lock", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO seen SELECT value FROM lcu WHERE pg_advisory_lock(7) IS NOT NULL FOR KEY SHARE",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE lcu SET value = 'three' WHERE id = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT pg_advisory_unlock(7)",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT value FROM seen",
                    expected: Expected::Rows {
                        columns: &[Column("value", TEXT)],
                        rows: &[
                            &[T("two")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT pg_advisory_unlock_all()",
                    expected: Expected::Rows {
                        columns: &[Column("pg_advisory_unlock_all", VOID)],
                        rows: &[
                            &[T("")],
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
            name: "an update waiting for a row of a table without a primary key that another transaction updated twice updates its newest version",
            set_up_script: &[
                "CREATE TABLE k (k int, balance int)",
                "INSERT INTO k VALUES (1, 160)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE k SET balance = balance + 10",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE k SET balance = balance - 100",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ UPDATE k SET balance = balance * 2",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT k, balance FROM k",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("balance", INT4)],
                        rows: &[
                            &[T("1"), T("140")],
                        ],
                        tag: "SELECT 1",
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
fn test_row_lock_waiters_queue() {
    run_scripts(&[
        ScriptTest {
            name: "transactions waiting for a row lock take it in the order they came",
            set_up_script: &[
                "CREATE TABLE tq (id int PRIMARY KEY, name text)",
                "INSERT INTO tq VALUES (1, 'a')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT id FROM tq WHERE id = 1 FOR SHARE",
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
                    query: "/* client B */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT id FROM tq WHERE id = 1 FOR UPDATE",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT id FROM tq WHERE id = 1 FOR UPDATE",
                    flow: Flow::Exec,
                    client: "C",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "C",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a transaction that holds a row lock upgrades it ahead of the transactions waiting for the row",
            set_up_script: &[
                "CREATE TABLE tq (id int PRIMARY KEY, name text)",
                "INSERT INTO tq VALUES (1, 'a')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT id FROM tq WHERE id = 1 FOR SHARE",
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
                    query: "/* client B */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT id FROM tq WHERE id = 1 FOR UPDATE",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT id FROM tq WHERE id = 1 FOR SHARE",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT id FROM tq WHERE id = 1 FOR UPDATE",
                    flow: Flow::Exec,
                    client: "C",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a FOR KEY SHARE lock stays on a row over another transaction update that changes no key",
            set_up_script: &[
                "CREATE TABLE tq (id int PRIMARY KEY, name text)",
                "INSERT INTO tq VALUES (1, 'a')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT id FROM tq WHERE id = 1 FOR KEY SHARE",
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
                    query: "/* client B */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT id FROM tq WHERE id = 1 FOR KEY SHARE",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE tq SET name = 'b' WHERE id = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT id FROM tq WHERE id = 1 FOR UPDATE",
                    flow: Flow::Exec,
                    client: "C",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "C",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_row_locks_of_catalog_rows() {
    run_scripts(&[
        ScriptTest {
            name: "row locks of system catalog rows against GRANT and REVOKE of their objects",
            set_up_script: &["CREATE TABLE cl (c INT)"],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT relhasindex FROM pg_class WHERE oid = 'cl'::regclass FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("relhasindex", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ GRANT SELECT ON cl TO PUBLIC;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT relacl IS NULL FROM pg_class WHERE oid = 'cl'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT relhasindex FROM pg_class WHERE oid = 'cl'::regclass FOR KEY SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("relhasindex", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ REVOKE SELECT ON cl FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT 1;",
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
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ GRANT SELECT ON cl TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT relhasindex FROM pg_class WHERE oid = 'cl'::regclass FOR UPDATE;",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT proname FROM pg_proc WHERE oid = 'now'::regproc FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME)],
                        rows: &[
                            &[T("now")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT proname FROM pg_proc WHERE oid = 'now'::regproc FOR UPDATE NOWAIT;",
                    expected: Expected::Error(Diagnostic { code: "55P03", message: "could not obtain lock on row in relation \"pg_proc\"", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ REVOKE SELECT ON cl FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT relhasindex FROM pg_class WHERE oid = 'cl'::regclass FOR UPDATE;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ GRANT SELECT ON cl TO PUBLIC;",
                    flow: Flow::Exec,
                    client: "C",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ SELECT relacl IS NULL FROM pg_class WHERE oid = 'cl'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "C",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "concurrent changes of one object other than GRANT wait for its lock and take the newest catalog",
            set_up_script: &["CREATE TABLE ca (c INT)"],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ALTER TABLE ca ADD COLUMN d INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ALTER TABLE ca ADD COLUMN e INT;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT attname FROM pg_attribute WHERE attrelid = 'ca'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME)],
                        rows: &[
                            &[T("c")],
                            &[T("d")],
                            &[T("e")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMENT ON TABLE ca IS 'first';",
                    expected: Expected::Tag("COMMENT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMENT ON TABLE ca IS 'second';",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT obj_description('ca'::regclass, 'pg_class');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("second")],
                        ],
                        tag: "SELECT 1",
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
fn test_key_share_through_running_no_key_updates() {
    run_scripts(&[
        ScriptTest {
            name: "FOR KEY SHARE takes a keyless row a running update changing no key is moving, without waiting",
            set_up_script: &[
                "CREATE TABLE dont_forget (value INT);",
                "INSERT INTO dont_forget VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dont_forget FOR KEY SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("value", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: "UPDATE dont_forget SET value = 2;",
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
                    query: "SELECT * FROM dont_forget FOR KEY SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("value", INT4)],
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
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a foreign key check waits for a running update moving the referenced row to another partition, then fails",
            set_up_script: &[
                "CREATE TABLE p (id INT, part INT) PARTITION BY LIST (part);",
                "CREATE TABLE p1 PARTITION OF p FOR VALUES IN (1);",
                "CREATE TABLE p2 PARTITION OF p FOR VALUES IN (2);",
                "ALTER TABLE p1 ADD UNIQUE (id);",
                "CREATE TABLE c (id INT REFERENCES p1 (id));",
                "INSERT INTO p VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET part = 2 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c VALUES (1);",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "tuple to be locked was already moved to another partition due to concurrent update", ..E }),
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
            ],
            ..S
        },
    ]);
}

#[test]
fn test_row_locks_of_outer_joins() {
    run_scripts(&[
        ScriptTest {
            name: "row locks of the nullable side of an outer join",
            set_up_script: &[
                "CREATE TABLE ra (id INT PRIMARY KEY, v TEXT);",
                "CREATE TABLE rb (id INT PRIMARY KEY, v TEXT);",
                "INSERT INTO ra VALUES (1, 'a'), (2, 'b');",
                "INSERT INTO rb VALUES (1, 'x');",
                "CREATE VIEW rv AS SELECT ra.id, rb.v FROM ra LEFT JOIN rb ON ra.id = rb.id;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ra.id, rb.v FROM ra LEFT JOIN rb ON ra.id = rb.id ORDER BY 1 FOR UPDATE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FOR UPDATE cannot be applied to the nullable side of an outer join", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ra.id, rb.v FROM ra LEFT JOIN rb ON ra.id = rb.id ORDER BY 1 FOR NO KEY UPDATE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FOR NO KEY UPDATE cannot be applied to the nullable side of an outer join", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ra.id, rb.v FROM ra LEFT JOIN rb ON ra.id = rb.id ORDER BY 1 FOR SHARE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FOR SHARE cannot be applied to the nullable side of an outer join", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ra.id, rb.v FROM ra LEFT JOIN rb ON ra.id = rb.id ORDER BY 1 FOR KEY SHARE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FOR KEY SHARE cannot be applied to the nullable side of an outer join", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ra.id, rb.v FROM ra LEFT JOIN rb ON ra.id = rb.id ORDER BY 1 FOR UPDATE OF ra;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ra.id, rb.v FROM ra LEFT JOIN rb ON ra.id = rb.id ORDER BY 1 FOR UPDATE OF rb;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FOR UPDATE cannot be applied to the nullable side of an outer join", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ra.id, rb.v FROM ra RIGHT JOIN rb ON ra.id = rb.id ORDER BY 1 FOR SHARE OF rb;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ra.id, rb.v FROM ra RIGHT JOIN rb ON ra.id = rb.id ORDER BY 1 FOR SHARE OF ra;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FOR SHARE cannot be applied to the nullable side of an outer join", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ra.id, rb.v FROM ra FULL JOIN rb ON ra.id = rb.id ORDER BY 1 FOR KEY SHARE OF ra;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FOR KEY SHARE cannot be applied to the nullable side of an outer join", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ra.id, rb.v FROM ra JOIN rb ON ra.id = rb.id FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM rv ORDER BY 1 FOR UPDATE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FOR UPDATE cannot be applied to the nullable side of an outer join", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ra.id FROM ra WHERE EXISTS (SELECT 1 FROM rb WHERE rb.id = ra.id) ORDER BY 1 FOR UPDATE;",
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
fn test_key_columns_of_unique_indexes() {
    run_scripts(&[
        ScriptTest {
            name: "a deferrable unique index's columns are key columns and a unique expression index's are not",
            set_up_script: &[
                "CREATE TABLE kd (id INT PRIMARY KEY, d INT UNIQUE DEFERRABLE, e TEXT, n INT);",
                "CREATE UNIQUE INDEX kd_lower ON kd (lower(e));",
                "INSERT INTO kd VALUES (1, 10, 'a', 0);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n FROM kd WHERE id = 1 FOR KEY SHARE;",
                    expected: Expected::Rows { columns: &[Column("n", INT4)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET lock_timeout = '2s';",
                    expected: Expected::Tag("SET"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE kd SET e = 'b' WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET lock_timeout;",
                    expected: Expected::Tag("RESET"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE kd SET d = 11 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    flow: Flow::Exec,
                    client: "b",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, d, e, n FROM kd;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", INT4), Column("e", TEXT), Column("n", INT4)],
                        rows: &[&[T("1"), T("11"), T("b"), T("0")]],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_on_conflict_update_of_a_row_committed_after_the_snapshot() {
    run_scripts(&[
        ScriptTest {
            name: "ON CONFLICT DO UPDATE in REPEATABLE READ fails on a row a transaction committed after the snapshot",
            set_up_script: &["CREATE TABLE kv (k INT PRIMARY KEY, v TEXT)"],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM kv;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kv VALUES (1, 'b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kv VALUES (1, 'a') ON CONFLICT (k) DO UPDATE SET v = kv.v || ' and a';",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kv VALUES (2, 'a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kv VALUES (2, 'a') ON CONFLICT (k) DO UPDATE SET v = kv.v || ' again';",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, v FROM kv ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("v", TEXT)],
                        rows: &[&[T("1"), T("b")], &[T("2"), T("a again")]],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_row_locks_follow_updates_in_progress() {
    run_scripts(&[
        ScriptTest {
            name: "a foreign key's check of a row of a table without a primary key waits for the transaction whose update in the row's partition the row's move to another partition followed, under READ COMMITTED, which ends with COMMIT",
            set_up_script: &[
                "CREATE TABLE foo_range_parted (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE foo_range_parted1 PARTITION OF foo_range_parted FOR VALUES FROM (1) TO (10);",
                "CREATE TABLE foo_range_parted2 PARTITION OF foo_range_parted FOR VALUES FROM (10) TO (20);",
                "INSERT INTO foo_range_parted VALUES (7, 'ABC');",
                "CREATE UNIQUE INDEX foo_range_parted1_a_unique ON foo_range_parted1 (a);",
                "CREATE TABLE bar (a INT REFERENCES foo_range_parted1 (a));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE foo_range_parted SET b = 'XYZ' WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE foo_range_parted SET a = 11 WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL READ COMMITTED;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bar VALUES (7);",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "tuple to be locked was already moved to another partition due to concurrent update", ..E }),
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
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM bar;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[], tag: "SELECT 0" },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a foreign key's check of a row of a table without a primary key waits for the transaction whose update in the row's partition the row's move to another partition followed, under READ COMMITTED, which ends with ROLLBACK",
            set_up_script: &[
                "CREATE TABLE foo_range_parted (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE foo_range_parted1 PARTITION OF foo_range_parted FOR VALUES FROM (1) TO (10);",
                "CREATE TABLE foo_range_parted2 PARTITION OF foo_range_parted FOR VALUES FROM (10) TO (20);",
                "INSERT INTO foo_range_parted VALUES (7, 'ABC');",
                "CREATE UNIQUE INDEX foo_range_parted1_a_unique ON foo_range_parted1 (a);",
                "CREATE TABLE bar (a INT REFERENCES foo_range_parted1 (a));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE foo_range_parted SET b = 'XYZ' WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE foo_range_parted SET a = 11 WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL READ COMMITTED;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bar VALUES (7);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM bar;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[&[T("7")]], tag: "SELECT 1" },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a foreign key's check of a row of a table without a primary key waits for the transaction whose update in the row's partition the row's move to another partition followed, under REPEATABLE READ, which ends with COMMIT",
            set_up_script: &[
                "CREATE TABLE foo_range_parted (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE foo_range_parted1 PARTITION OF foo_range_parted FOR VALUES FROM (1) TO (10);",
                "CREATE TABLE foo_range_parted2 PARTITION OF foo_range_parted FOR VALUES FROM (10) TO (20);",
                "INSERT INTO foo_range_parted VALUES (7, 'ABC');",
                "CREATE UNIQUE INDEX foo_range_parted1_a_unique ON foo_range_parted1 (a);",
                "CREATE TABLE bar (a INT REFERENCES foo_range_parted1 (a));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE foo_range_parted SET b = 'XYZ' WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE foo_range_parted SET a = 11 WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bar VALUES (7);",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
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
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM bar;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[], tag: "SELECT 0" },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a foreign key's check of a row of a table without a primary key waits for the transaction whose update in the row's partition the row's move to another partition followed, under REPEATABLE READ, which ends with ROLLBACK",
            set_up_script: &[
                "CREATE TABLE foo_range_parted (a INT, b TEXT) PARTITION BY RANGE (a);",
                "CREATE TABLE foo_range_parted1 PARTITION OF foo_range_parted FOR VALUES FROM (1) TO (10);",
                "CREATE TABLE foo_range_parted2 PARTITION OF foo_range_parted FOR VALUES FROM (10) TO (20);",
                "INSERT INTO foo_range_parted VALUES (7, 'ABC');",
                "CREATE UNIQUE INDEX foo_range_parted1_a_unique ON foo_range_parted1 (a);",
                "CREATE TABLE bar (a INT REFERENCES foo_range_parted1 (a));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE foo_range_parted SET b = 'XYZ' WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE foo_range_parted SET a = 11 WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO bar VALUES (7);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM bar;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[&[T("7")]], tag: "SELECT 1" },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "FOR KEY SHARE on a row of a table keyed by a deferrable primary key waits for the transaction whose update in the row's partition the row's move to another partition followed, under READ COMMITTED, which ends with COMMIT",
            set_up_script: &[
                "CREATE TABLE d (a INT, b TEXT, PRIMARY KEY (a) DEFERRABLE) PARTITION BY RANGE (a);",
                "CREATE TABLE d1 PARTITION OF d FOR VALUES FROM (1) TO (10);",
                "CREATE TABLE d2 PARTITION OF d FOR VALUES FROM (10) TO (20);",
                "INSERT INTO d VALUES (7, 'ABC');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET b = 'XYZ' WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET a = 11 WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL READ COMMITTED;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 FROM ONLY d1 x WHERE a = 7 FOR KEY SHARE OF x;",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "tuple to be locked was already moved to another partition due to concurrent update", ..E }),
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
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM d;",
                    expected: Expected::Rows { columns: &[Column("a", INT4), Column("b", TEXT)], rows: &[&[T("11"), T("XYZ")]], tag: "SELECT 1" },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "FOR KEY SHARE on a row of a table keyed by a deferrable primary key waits for the transaction whose update in the row's partition the row's move to another partition followed, under READ COMMITTED, which ends with ROLLBACK",
            set_up_script: &[
                "CREATE TABLE d (a INT, b TEXT, PRIMARY KEY (a) DEFERRABLE) PARTITION BY RANGE (a);",
                "CREATE TABLE d1 PARTITION OF d FOR VALUES FROM (1) TO (10);",
                "CREATE TABLE d2 PARTITION OF d FOR VALUES FROM (10) TO (20);",
                "INSERT INTO d VALUES (7, 'ABC');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET b = 'XYZ' WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET a = 11 WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL READ COMMITTED;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 FROM ONLY d1 x WHERE a = 7 FOR KEY SHARE OF x;",
                    expected: Expected::Rows { columns: &[Column("?column?", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM d;",
                    expected: Expected::Rows { columns: &[Column("a", INT4), Column("b", TEXT)], rows: &[&[T("7"), T("ABC")]], tag: "SELECT 1" },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "FOR KEY SHARE on a row of a table keyed by a deferrable primary key waits for the transaction whose update in the row's partition the row's move to another partition followed, under REPEATABLE READ, which ends with COMMIT",
            set_up_script: &[
                "CREATE TABLE d (a INT, b TEXT, PRIMARY KEY (a) DEFERRABLE) PARTITION BY RANGE (a);",
                "CREATE TABLE d1 PARTITION OF d FOR VALUES FROM (1) TO (10);",
                "CREATE TABLE d2 PARTITION OF d FOR VALUES FROM (10) TO (20);",
                "INSERT INTO d VALUES (7, 'ABC');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET b = 'XYZ' WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET a = 11 WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 FROM ONLY d1 x WHERE a = 7 FOR KEY SHARE OF x;",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
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
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM d;",
                    expected: Expected::Rows { columns: &[Column("a", INT4), Column("b", TEXT)], rows: &[&[T("11"), T("XYZ")]], tag: "SELECT 1" },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "FOR KEY SHARE on a row of a table keyed by a deferrable primary key waits for the transaction whose update in the row's partition the row's move to another partition followed, under REPEATABLE READ, which ends with ROLLBACK",
            set_up_script: &[
                "CREATE TABLE d (a INT, b TEXT, PRIMARY KEY (a) DEFERRABLE) PARTITION BY RANGE (a);",
                "CREATE TABLE d1 PARTITION OF d FOR VALUES FROM (1) TO (10);",
                "CREATE TABLE d2 PARTITION OF d FOR VALUES FROM (10) TO (20);",
                "INSERT INTO d VALUES (7, 'ABC');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET b = 'XYZ' WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE d SET a = 11 WHERE a = 7;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 FROM ONLY d1 x WHERE a = 7 FOR KEY SHARE OF x;",
                    expected: Expected::Rows { columns: &[Column("?column?", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM d;",
                    expected: Expected::Rows { columns: &[Column("a", INT4), Column("b", TEXT)], rows: &[&[T("7"), T("ABC")]], tag: "SELECT 1" },
                    client: "A",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_a_lock_on_a_row_deleted_after_the_snapshot_keeps_nothing() {
    run_scripts(&[
        ScriptTest {
            name: "READ COMMITTED FOR UPDATE of a row a commit deleted returns nothing and keeps no lock on its key",
            set_up_script: &[
                "CREATE TABLE fud (id INT PRIMARY KEY, v TEXT)",
                "INSERT INTO fud VALUES (1, 'a')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DELETE FROM fud WHERE id = 1",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL READ COMMITTED",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT id FROM fud WHERE id = 1 FOR UPDATE",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ INSERT INTO fud VALUES (1, 'new')",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client C */ UPDATE fud SET v = 'newer' WHERE id = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "C",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT id, v FROM fud",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("newer")],
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
