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
fn test_dolt_backup() {
    run_scripts(&[
        ScriptTest {
            name: "sync-url restore preserves data and commit history",
            set_up_script: &[
                "create table items (id int primary key, label text not null);",
                "insert into items values (1, 'apple'), (2, 'banana');",
                "select dolt_commit('-Am', 'first commit: add apple and banana');",
                "insert into items values (3, 'cherry');",
                "select dolt_commit('-Am', 'second commit: add cherry');",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_syncurl');",
                "\\c restored_syncurl",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, label from items order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("label", TEXT)],
                        rows: &[
                            &[T("1"), T("apple")],
                            &[T("2"), T("banana")],
                            &[T("3"), T("cherry")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select count(*) from dolt.commits;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select message from dolt.commits order by date;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("CREATE DATABASE")],
                            &[T("first commit: add apple and banana")],
                            &[T("second commit: add cherry")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' output for the query.
                ScriptTestAssertion {
                    query: "select column_name, is_nullable from information_schema.columns where table_name = 'items' and table_schema = 'public' order by column_name;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("id"), T("NO")],
                            &[T("label"), T("NO")],
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
fn test_dolt_backup_2() {
    run_scripts(&[
        ScriptTest {
            name: "named backup sync restore preserves data and commit history",
            set_up_script: &[
                "create table orders (id int primary key, item text, qty int);",
                "insert into orders values (1, 'widget', 10);",
                "select dolt_add('.');",
                "select dolt_commit('-m', 'initial order');",
                "insert into orders values (2, 'gadget', 5);",
                "select dolt_add('.');",
                "select dolt_commit('-m', 'second order');",
                "select dolt_backup('add', 'named_bak', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('sync', 'named_bak');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_named');",
                "\\c restored_named",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, item, qty from orders order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("item", TEXT), Column("qty", INT4)],
                        rows: &[
                            &[T("1"), T("widget"), T("10")],
                            &[T("2"), T("gadget"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select count(*) from dolt.commits;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select message from dolt.commits order by date;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Initialize data repository")],
                            &[T("CREATE DATABASE")],
                            &[T("initial order")],
                            &[T("second order")],
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
fn test_dolt_backup_3() {
    run_scripts(&[
        ScriptTest {
            name: "dolt.dolt_backups table reflects add and remove",
            set_up_script: &[
                "select dolt_backup('add', 'backup_a', 'file://{TEMPDIR}/backup_a');",
                "select dolt_backup('add', 'backup_b', 'file://{TEMPDIR}/backup_b');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select name from dolt.dolt_backups order by name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("backup_a")],
                            &[T("backup_b")],
                        ],
                        tag: "SELECT 2",
                    },
                    skip: Some("Doltgres names its system tables dolt.dolt_* in the dolt schema, where Doltgrust's dolt schema names them without the dolt_ prefix, as dolt.backups (Appendix C)"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_backup('remove', 'backup_a');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select name from dolt.dolt_backups order by name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("backup_b")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("Doltgres names its system tables dolt.dolt_* in the dolt schema, where Doltgrust's dolt schema names them without the dolt_ prefix, as dolt.backups (Appendix C)"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_backup('remove', 'backup_b');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select count(*) from dolt.dolt_backups;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("Doltgres names its system tables dolt.dolt_* in the dolt schema, where Doltgrust's dolt schema names them without the dolt_ prefix, as dolt.backups (Appendix C)"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_4() {
    run_scripts(&[
        ScriptTest {
            name: "restore --force overwrites an existing database and data is correct",
            set_up_script: &[
                "create table t (id int primary key, v text);",
                "insert into t values (1, 'original');",
                "select dolt_add('.');",
                "select dolt_commit('-m', 'seed');",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'db_to_overwrite');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'db_to_overwrite');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "database 'db_to_overwrite' already exists, use '--force' to overwrite", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_backup('restore', '--force', 'file://{TEMPDIR}/backup', 'db_to_overwrite');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "USE db_to_overwrite;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, v from t order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("original")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select count(*) from dolt.commits;",
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
                    query: "USE postgres;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "drop database db_to_overwrite;",
                    expected: Expected::Tag("DROP DATABASE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_5() {
    run_scripts(&[
        ScriptTest {
            name: "multi-branch backup and restore preserves all branches",
            set_up_script: &[
                "create table products (id int primary key, name text);",
                "insert into products values (1, 'alpha');",
                "select dolt_add('.');",
                "select dolt_commit('-m', 'add alpha on main');",
                "select dolt_branch('feature');",
                "select dolt_checkout('feature');",
                "insert into products values (2, 'beta');",
                "select dolt_commit('-Am', 'add beta on feature');",
                "select dolt_checkout('main');",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_branches');",
                "\\c restored_branches",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, name from products order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("alpha")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select name from dolt.branches order by name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("feature")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_checkout('feature');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, name from products order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("alpha")],
                            &[T("2"), T("beta")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select message from dolt.commits order by date desc limit 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("add beta on feature")],
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
fn test_dolt_backup_6() {
    run_scripts(&[
        ScriptTest {
            name: "working set state is preserved in backup",
            set_up_script: &[
                "create table logs (id int primary key, msg text);",
                "insert into logs values (1, 'committed');",
                "select dolt_add('.');",
                "select dolt_commit('-m', 'first commit');",
                "insert into logs values (2, 'staged only');",
                "select dolt_add('.');",
                "insert into logs values (3, 'unstaged');",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_workingset');",
                "\\c restored_workingset",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, msg from logs order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("msg", TEXT)],
                        rows: &[
                            &[T("1"), T("committed")],
                            &[T("2"), T("staged only")],
                            &[T("3"), T("unstaged")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select count(*) from dolt.commits;",
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
fn test_dolt_backup_7() {
    run_scripts(&[
        ScriptTest {
            name: "incremental sync captures new commits added after the first sync",
            set_up_script: &[
                "select dolt_backup('add', 'incr_bak', 'file://{TEMPDIR}/backup');",
                "create table events (id int primary key, name text);",
                "insert into events values (1, 'first');",
                "select dolt_add('.');",
                "select dolt_commit('-m', 'first event');",
                "select dolt_backup('sync', 'incr_bak');",
                "insert into events values (2, 'second');",
                "select dolt_add('.');",
                "select dolt_commit('-m', 'second event');",
                "select dolt_backup('sync', 'incr_bak');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_incr');",
                "select dolt_backup('remove', 'incr_bak');",
                "\\c restored_incr",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, name from events order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("first")],
                            &[T("2"), T("second")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select count(*) from dolt.commits;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
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
fn test_dolt_backup_8() {
    run_scripts(&[
        ScriptTest {
            name: "sync nonexistent named backup returns error",
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_backup('sync', 'does_not_exist');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "backup 'does_not_exist' not found", ..E }),
                    skip: Some("Doltgrust refuses a missing named backup with Dolt's 22023, where the Doltgres expectation is XX000"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_9() {
    run_scripts(&[
        ScriptTest {
            name: "remove nonexistent backup returns error",
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_backup('remove', 'no_such_backup');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "backup 'no_such_backup' not found", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_10() {
    run_scripts(&[
        ScriptTest {
            name: "restore from nonexistent URL returns error",
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_backup('restore', 'file://{TESTDATA}/csv-load-basic-cases.sql/backup', 'new_db');",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "failed to create directory '", message_contains: true, ..E }),
                    skip: Some("Doltgrust refuses a restore from a missing directory with Dolt's 22023 message, where the Doltgres expectation is its own XX000 failed to create directory"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_11() {
    run_scripts(&[
        ScriptTest {
            name: "adding a backup with a duplicate name returns error",
            set_up_script: &[
                "select dolt_backup('add', 'mybackup', 'file://{TEMPDIR}/backup_a');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_backup('add', 'mybackup', 'file://{TEMPDIR}/backup_b');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "backup 'mybackup' already exists", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_backup('remove', 'mybackup');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_backup", INT8)],
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
fn test_dolt_backup_12() {
    run_scripts(&[
        ScriptTest {
            name: "unknown dolt_backup operation returns error",
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_backup('typo', 'name', 'url');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized dolt_backup parameter 'typo'", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_13() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_backup with no args returns usage error",
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select dolt_backup();",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "use 'dolt_backups' table to list backups", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_14() {
    run_scripts(&[
        ScriptTest {
            name: "empty database backup and restore",
            set_up_script: &[
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_empty');",
                "\\c restored_empty",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select count(*) from information_schema.tables where table_schema = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' output for the query.
                ScriptTestAssertion {
                    query: "select count(*) >= 2 from dolt.commits;",
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
fn test_dolt_backup_15() {
    run_scripts(&[
        ScriptTest {
            name: "standalone sequence state is preserved across backup and restore",
            set_up_script: &[
                "create sequence counter start 1 increment 5;",
                "select nextval('counter');",
                "select nextval('counter');",
                "select nextval('counter');",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_seq');",
                "\\c restored_seq",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' output for the query.
                ScriptTestAssertion {
                    query: "select sequence_name from information_schema.sequences where sequence_schema = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("sequence_name", NAME)],
                        rows: &[
                            &[T("counter")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select nextval('counter');",
                    expected: Expected::Rows {
                        columns: &[Column("nextval", INT8)],
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
fn test_dolt_backup_16() {
    run_scripts(&[
        ScriptTest {
            name: "serial column sequence state is preserved across backup and restore",
            set_up_script: &[
                "create table widgets (id serial primary key, name text);",
                "insert into widgets (name) values ('a'), ('b'), ('c');",
                "select dolt_add('.');",
                "select dolt_commit('-m', 'seed widgets');",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_serial');",
                "\\c restored_serial",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, name from widgets order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                            &[T("3"), T("c")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "insert into widgets (name) values ('d') returning id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("4")],
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
fn test_dolt_backup_17() {
    run_scripts(&[
        ScriptTest {
            name: "views are preserved across backup and restore",
            set_up_script: &[
                "create table employees (id int primary key, dept text, salary int);",
                "insert into employees values (1, 'eng', 100000), (2, 'eng', 120000), (3, 'ops', 90000);",
                "create view eng_employees as select id, salary from employees where dept = 'eng';",
                "select dolt_add('.');",
                "select dolt_commit('-m', 'add employees and view');",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_views');",
                "\\c restored_views",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' output for the query.
                ScriptTestAssertion {
                    query: "select table_name from information_schema.views where table_schema = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("eng_employees")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, salary from eng_employees order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("salary", INT4)],
                        rows: &[
                            &[T("1"), T("100000")],
                            &[T("2"), T("120000")],
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
fn test_dolt_backup_18() {
    run_scripts(&[
        ScriptTest {
            name: "foreign key constraints are preserved and enforced after restore",
            set_up_script: &[
                "create table categories (id int primary key, name text);",
                "create table items (id int primary key, cat_id int references categories(id), label text);",
                "insert into categories values (1, 'fruit'), (2, 'veg');",
                "insert into items values (1, 1, 'apple'), (2, 2, 'carrot');",
                "select dolt_add('.');",
                "select dolt_commit('-m', 'seed fk data');",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_fk');",
                "\\c restored_fk",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, label from items order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("label", TEXT)],
                        rows: &[
                            &[T("1"), T("apple")],
                            &[T("2"), T("carrot")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "insert into items values (3, 99, 'mystery');",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "items" violates foreign key constraint "items_cat_id_fkey""#, detail: r#"Key (cat_id)=(99) is not present in table "categories"."#, schema: "public", table: "items", constraint: "items_cat_id_fkey", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select count(*) from information_schema.referential_constraints where constraint_schema = 'public';",
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
fn test_dolt_backup_19() {
    run_scripts(&[
        ScriptTest {
            name: "secondary index is preserved and used after restore",
            set_up_script: &[
                "create table products (id int primary key, sku text not null, price int);",
                "create index idx_sku on products (sku);",
                "insert into products values (1, 'AAA', 10), (2, 'BBB', 20), (3, 'CCC', 30);",
                "select dolt_add('.');",
                "select dolt_commit('-m', 'seed products with index');",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_idx');",
                "\\c restored_idx",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select indexname from pg_indexes where tablename = 'products' and indexname = 'idx_sku';",
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("idx_sku")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, sku, price from products order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("sku", TEXT), Column("price", INT4)],
                        rows: &[
                            &[T("1"), T("AAA"), T("10")],
                            &[T("2"), T("BBB"), T("20")],
                            &[T("3"), T("CCC"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, price from products where sku = 'BBB';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("price", INT4)],
                        rows: &[
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "insert into products values (4, 'AAA', 99);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id from products where sku = 'AAA' order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("4")],
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
fn test_dolt_backup_20() {
    run_scripts(&[
        ScriptTest {
            name: "custom enum type is preserved across backup and restore",
            set_up_script: &[
                "create type my_mood as enum ('happy', 'ok', 'sad');",
                "create table user_states (id int primary key, name text, mood my_mood);",
                "insert into user_states values (1, 'alice', 'happy'), (2, 'bob', 'sad');",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_types');",
                "\\c restored_types",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select typname from pg_type where typname = 'my_mood';",
                    expected: Expected::Rows {
                        columns: &[Column("typname", NAME)],
                        rows: &[
                            &[T("my_mood")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select 'ok'::my_mood;",
                    expected: Expected::Rows {
                        columns: &[Column("my_mood", USER_DEFINED)],
                        rows: &[
                            &[T("ok")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, name, mood::text from user_states order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT), Column("mood", TEXT)],
                        rows: &[
                            &[T("1"), T("alice"), T("happy")],
                            &[T("2"), T("bob"), T("sad")],
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
fn test_dolt_backup_21() {
    run_scripts(&[
        ScriptTest {
            name: "domain type is preserved across backup and restore",
            set_up_script: &[
                "create domain pos_int as integer check (value > 0);",
                "create table measurements (id int primary key, val pos_int);",
                "insert into measurements values (1, 42);",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_domain');",
                "\\c restored_domain",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select typname, typtype from pg_type where typname = 'pos_int';",
                    expected: Expected::Rows {
                        columns: &[Column("typname", NAME), Column("typtype", CHAR)],
                        rows: &[
                            &[T("pos_int"), T("d")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select val from measurements where id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("val", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' output for the query.
                ScriptTestAssertion {
                    query: "insert into measurements values (2, -1);",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"value for domain pos_int violates check constraint "pos_int_check""#, schema: "public", data_type: "pos_int", constraint: "pos_int_check", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_22() {
    run_scripts(&[
        ScriptTest {
            name: "user-defined composite type is preserved across backup and restore",
            set_up_script: &[
                "create type point_t as (x float8, y float8);",
                r#"create function distance(p point_t) returns float8 as $$
			 begin return sqrt((p).x * (p).x + (p).y * (p).y); end;
			 $$ language plpgsql;"#,
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_composite');",
                "\\c restored_composite",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select typname, typtype from pg_type where typname = 'point_t';",
                    expected: Expected::Rows {
                        columns: &[Column("typname", NAME), Column("typtype", CHAR)],
                        rows: &[
                            &[T("point_t"), T("c")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' output for the query.
                ScriptTestAssertion {
                    query: "select (row(3.0, 4.0)::point_t).x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", FLOAT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select distance(row(3.0, 4.0)::point_t);",
                    expected: Expected::Rows {
                        columns: &[Column("distance", FLOAT8)],
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
fn test_dolt_backup_23() {
    run_scripts(&[
        ScriptTest {
            name: "user-defined function is preserved across backup and restore",
            set_up_script: &[
                r#"create function double_it(n int) returns int as $$
			 begin return n * 2; end;
			 $$ language plpgsql;"#,
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_funcs');",
                "\\c restored_funcs",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select double_it(21);",
                    expected: Expected::Rows {
                        columns: &[Column("double_it", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select double_it(0);",
                    expected: Expected::Rows {
                        columns: &[Column("double_it", INT4)],
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
fn test_dolt_backup_24() {
    run_scripts(&[
        ScriptTest {
            name: "stored procedure is preserved across backup and restore",
            set_up_script: &[
                "create table job_log (id int primary key, status text);",
                "insert into job_log values (1, 'pending');",
                r#"create procedure mark_done(job_id int) as $$
			 begin update job_log set status = 'done' where id = job_id; end;
			 $$ language plpgsql;"#,
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_procs');",
                "\\c restored_procs",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "call mark_done(1);",
                    expected: Expected::Tag("CALL"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select status from job_log where id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("status", TEXT)],
                        rows: &[
                            &[T("done")],
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
fn test_dolt_backup_25() {
    run_scripts(&[
        ScriptTest {
            name: "trigger is preserved and fires after restore",
            set_up_script: &[
                "create table readings (id int primary key, val int);",
                r#"create function clamp_val() returns trigger as $$
			 begin
			   if NEW.val > 100 then NEW.val := 100; end if;
			   return NEW;
			 end;
			 $$ language plpgsql;"#,
                "create trigger clamp_trigger before insert on readings for each row execute function clamp_val();",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_triggers');",
                "\\c restored_triggers",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "insert into readings values (1, 200);",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select val from readings where id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("val", INT4)],
                        rows: &[
                            &[T("100")],
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
fn test_dolt_backup_26() {
    run_scripts(&[
        ScriptTest {
            name: "extension is preserved across backup and restore",
            set_up_script: &[
                r#"create extension "uuid-ossp";"#,
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_ext');",
                "\\c restored_ext",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' output for the query.
                ScriptTestAssertion {
                    query: "select extname, extversion from pg_catalog.pg_extension;",
                    expected: Expected::Rows {
                        columns: &[Column("extname", NAME), Column("extversion", TEXT)],
                        rows: &[
                            &[T("plpgsql"), T("1.0")],
                            &[T("uuid-ossp"), T("1.1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' output for the query.
                ScriptTestAssertion {
                    query: "select length(uuid_generate_v4()::text) = 36;",
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
fn test_dolt_backup_27() {
    run_scripts(&[
        ScriptTest {
            name: "named schema contents are preserved across backup and restore",
            set_up_script: &[
                "create schema inventory;",
                "create table inventory.products (id int primary key, name text);",
                "insert into inventory.products values (1, 'widget'), (2, 'gadget');",
                "select dolt_commit('-Am', 'add inventory schema');",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_schema');",
                "\\c restored_schema",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' output for the query.
                ScriptTestAssertion {
                    query: "select schema_name from information_schema.schemata where schema_name = 'inventory';",
                    expected: Expected::Rows {
                        columns: &[Column("schema_name", NAME)],
                        rows: &[
                            &[T("inventory")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "select id, name from inventory.products order by id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("widget")],
                            &[T("2"), T("gadget")],
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
fn test_dolt_backup_28() {
    run_scripts(&[
        ScriptTest {
            name: "custom cast is preserved across backup and restore",
            set_up_script: &[
                "CREATE TABLE cast_src (v text);",
                "CREATE TABLE cast_dst (v text, tag text);",
                r#"CREATE FUNCTION cast_src_to_dst(src cast_src) RETURNS cast_dst AS $$
			 SELECT ROW((src).v, 'casted')::cast_dst
			 $$ LANGUAGE SQL;"#,
                r#"CREATE FUNCTION cast_verify(dst cast_dst) RETURNS text AS $$
			 SELECT (dst).v || ':' || (dst).tag
			 $$ LANGUAGE SQL;"#,
                "CREATE CAST (cast_src AS cast_dst) WITH FUNCTION cast_src_to_dst(cast_src);",
                "select dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "select dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored_casts');",
                "\\c restored_casts",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so this expectation follows Postgres' column naming.
                ScriptTestAssertion {
                    query: r#"SELECT c.castcontext::text, c.castmethod::text
					FROM pg_cast c
					JOIN pg_type src ON src.oid = c.castsource
					JOIN pg_type dst ON dst.oid = c.casttarget
					WHERE src.typname = 'cast_src' AND dst.typname = 'cast_dst';"#,
                    expected: Expected::Rows {
                        columns: &[Column("castcontext", TEXT), Column("castmethod", TEXT)],
                        rows: &[
                            &[T("e"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT cast_verify((ROW('hello')::cast_src)::cast_dst);",
                    expected: Expected::Rows {
                        columns: &[Column("cast_verify", TEXT)],
                        rows: &[
                            &[T("hello:casted")],
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
fn test_dolt_backups_in_the_dolt_schema_drops_the_prefix() {
    run_scripts(&[
        ScriptTest {
            name: "dolt.backups shows the backups dolt_backup adds and removes",
            set_up_script: &[
                "SELECT dolt_backup('add', 'backup_a', 'file://{TEMPDIR}/backup_a');",
                "SELECT dolt_backup('add', 'backup_b', 'file://{TEMPDIR}/backup_b');",
                "SELECT dolt_backup('remove', 'backup_a');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT name, params FROM dolt.backups ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("params", JSONB)],
                        rows: &[
                            &[T("backup_b"), T("{}")],
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
fn test_dolt_backup_keeps_a_merge_in_progress() {
    run_scripts(&[
        ScriptTest {
            name: "a branch's merge with conflicts survives a backup and its restore",
            set_up_script: &[
                "CREATE TABLE t (pk INT PRIMARY KEY, v INT);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'base');",
                "SELECT dolt_branch('other');",
                "UPDATE t SET v = 2;",
                "SELECT dolt_commit('-am', 'ours');",
                "UPDATE \"postgres/other\".public.t SET v = 3;",
                "SELECT dolt_commit('--branch', 'other', '-am', 'theirs');",
                "SET dolt_allow_commit_conflicts = on;",
                "SELECT dolt_merge('other');",
                "SELECT dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "SELECT dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored');",
                "\\c restored",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT is_merging, source FROM dolt_merge_status;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT)],
                        rows: &[&[T("t"), T("other")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT \"table\", num_conflicts FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[&[T("public.t"), T("1")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT our_v, their_v FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("our_v", INT4), Column("their_v", INT4)],
                        rows: &[&[T("2"), T("3")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[&[T(""), T("0"), T("0"), T("merge aborted")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT v FROM t;",
                    expected: Expected::Rows { columns: &[Column("v", INT4)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_prune_with_grace_period() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_backup --prune-with-grace-period checks its duration and prunes nothing a recent sync wrote",
            intent: "Dolt's sql-backup.bats: --prune-with-grace-period rejects bad values and unrelated subcommands, and leaves a recently written destination alone.",
            set_up_script: &[
                "SELECT dolt_backup('add', 'b1', 'file://{TEMPDIR}/backup');",
                "SELECT dolt_backup('sync', 'b1');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_backup('sync', 'b1', '--prune-with-grace-period', '5m');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "--prune-with-grace-period must be at least 10m0s, got 5m0s", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_backup('sync', 'b1', '--prune-with-grace-period', 'nonsense');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid value for --prune-with-grace-period: time: invalid duration \"nonsense\"", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_backup('add', 'b2', 'file://{TEMPDIR}/backup-2', '--prune-with-grace-period', '1h');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "--prune-with-grace-period is only supported with 'sync' and 'sync-url'", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_backup('remove', 'b1', '--prune-with-grace-period', '1h');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "--prune-with-grace-period is only supported with 'sync' and 'sync-url'", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t (a INT PRIMARY KEY);",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-Am', 'cm')) > 0;",
                    expected: Expected::Rows { columns: &[Column("?column?", BOOL)], rows: &[&[T("t")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_backup('sync', 'b1', '--prune-with-grace-period', '1h');",
                    expected: Expected::Rows { columns: &[Column("dolt_backup", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored');",
                    expected: Expected::Rows { columns: &[Column("dolt_backup", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    client: "s2",
                    database: "restored",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_keeps_stashes() {
    run_scripts(&[
        ScriptTest {
            name: "a stash survives a backup and its restore, and pops there",
            intent: "Dolt's backup mirrors every ref, so dolt_stashes of a restored backup lists the stashes the database had.",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'one');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_stash('push', 's1');",
                "SELECT dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "SELECT dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored');",
                "\\c restored",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's output is expected.
                ScriptTestAssertion {
                    query: "SELECT name, stash_id, branch, commit_message FROM dolt_stashes;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("stash_id", TEXT), Column("branch", TEXT), Column("commit_message", TEXT)],
                        rows: &[&[T("s1"), T("stash@{0}"), T("main"), T("one")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_stash('pop', 's1');",
                    expected: Expected::Rows { columns: &[Column("dolt_stash", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM t;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_restore_of_a_url_without_a_backup() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_backup restore of a directory that is missing or holds no backup refuses as Dolt does",
            set_up_script: &[],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Dolt's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_backup('restore', 'file://{TEMPDIR}/missing', 'r1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "not found", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_backup('restore', 'file://{TEMPDIR}', 'r2');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_database WHERE datname IN ('r1', 'r2');",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_sync_in_a_transaction_that_changed_a_definition() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_backup sync inside a transaction that altered a table backs up the committed table",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "CREATE TABLE u (a INT PRIMARY KEY, b TEXT);",
                "INSERT INTO u VALUES (1, '1');",
                "SELECT dolt_commit('-Am', 'one');",
            ],
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", flow: Flow::Exec, ..A },
                ScriptTestAssertion { query: "ALTER TABLE t ADD COLUMN b INT;", flow: Flow::Exec, ..A },
                ScriptTestAssertion { query: "INSERT INTO t VALUES (2, 2);", flow: Flow::Exec, ..A },
                ScriptTestAssertion { query: "ALTER TABLE u ALTER COLUMN b TYPE INT USING b::INT;", flow: Flow::Exec, ..A },
                ScriptTestAssertion { query: "TRUNCATE t;", flow: Flow::Exec, ..A },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                    expected: Expected::Rows { columns: &[Column("dolt_backup", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", flow: Flow::Exec, ..A },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_backup('restore', 'file://{TEMPDIR}/backup', 'restored');",
                    expected: Expected::Rows { columns: &[Column("dolt_backup", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "restored",
                    database: "restored",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT pg_typeof(b)::TEXT AS b FROM u;",
                    expected: Expected::Rows { columns: &[Column("b", TEXT)], rows: &[&[T("text")]], tag: "SELECT 1" },
                    client: "restored",
                    database: "restored",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_backup_restore_with_force_ends_the_sessions_of_the_database_it_replaces() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_backup restore with --force ends the other sessions of the database it replaces, as DROP DATABASE with FORCE does",
            set_up_script: &[
                "CREATE TABLE t (a INT PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-Am', 'one');",
                "SELECT dolt_backup('sync-url', 'file://{TEMPDIR}/backup');",
                "CREATE DATABASE restored;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 1 AS one;",
                    expected: Expected::Rows { columns: &[Column("one", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "restored",
                    database: "restored",
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_backup('restore', '--force', 'file://{TEMPDIR}/backup', 'restored');",
                    expected: Expected::Rows { columns: &[Column("dolt_backup", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_stat_activity WHERE datname = 'restored';",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("0")]], tag: "SELECT 1" },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so Dolt's behavior is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows { columns: &[Column("a", INT4)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "fresh",
                    database: "restored",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
