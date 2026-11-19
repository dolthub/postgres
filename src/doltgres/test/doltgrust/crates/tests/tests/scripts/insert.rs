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
fn test_insert() {
    run_scripts(&[
        ScriptTest {
            name: "simple insert",
            set_up_script: &[
                "CREATE TABLE mytable (id INT PRIMARY KEY, name TEXT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO mytable (id, name) VALUES (1, 'hello')",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable (ID, naME) VALUES (2, 'world')",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytable order by id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("hello")],
                            &[T("2"), T("world")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless insert",
            set_up_script: &[
                "CREATE TABLE mytable (id INT, name TEXT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO mytable (id, name) VALUES (1, 'hello')",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable (ID, naME) VALUES (2, 'world')",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytable order by id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("hello")],
                            &[T("2"), T("world")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "on conflict clause",
            set_up_script: &[
                "CREATE TABLE mytable (id INT primary key, name TEXT)",
                "create table t2 (id int primary key, c1 text, c2 text)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO mytable (id, name) VALUES (1, 'hello')",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable (ID, naME) VALUES (2, 'world')",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable (ID, naME) VALUES (1, 'world') ON CONFLICT (id) DO UPDATE SET name = 'world'",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable (ID, naME) VALUES (2, 'hello') ON CONFLICT (id) DO UPDATE SET name = 'conflict'",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable (ID, naME) VALUES (1, 'not inserted') ON CONFLICT (id) DO NOTHING",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytable order by id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("world")],
                            &[T("2"), T("conflict")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mytable (ID, naME) VALUES (1, 'hello') ON CONFLICT (id) DO UPDATE set name = concat('new', name)",
                    expected: Expected::Error(Diagnostic { code: "42702", message: r#"column reference "name" is ambiguous"#, position: 104, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mytable order by id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("world")],
                            &[T("2"), T("conflict")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 (id, c1, c2) VALUES (1, 'hello', 'world'), (2, 'world', 'hello')",
                    expected: Expected::Tag("INSERT 0 2"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 (id, c1, c2) VALUES (1, 'hello', 'world') ON CONFLICT (id) DO UPDATE SET c1 = 'conflict', c2 = c1",
                    expected: Expected::Error(Diagnostic { code: "42702", message: r#"column reference "c1" is ambiguous"#, position: 111, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 (id, c1, c2) VALUES (2, 'hello', 'world') ON CONFLICT (id) DO UPDATE SET c2 = c1",
                    expected: Expected::Error(Diagnostic { code: "42702", message: r#"column reference "c1" is ambiguous"#, position: 94, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 order by id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c1", TEXT), Column("c2", TEXT)],
                        rows: &[
                            &[T("1"), T("hello"), T("world")],
                            &[T("2"), T("world"), T("hello")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t2 (id, c1, c2) 
VALUES ($1, $2, $3)
ON CONFLICT (id) do update set c1 = $4"#,
                    bind_vars: &[BindVar::Int(1), BindVar::Str("x"), BindVar::Str("y"), BindVar::Str("no conflict expected")],
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conditional on conflict update",
            set_up_script: &[
                "CREATE TABLE conditional_upsert (id INT PRIMARY KEY, version INT, note TEXT)",
                "INSERT INTO conditional_upsert VALUES (1, 5, 'original'), (2, 1, 'second')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO conditional_upsert VALUES (1, 6, 'advanced') ON CONFLICT (id) DO UPDATE SET version = 6, note = 'advanced' WHERE conditional_upsert.version < 6",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conditional_upsert VALUES (1, 6, 'advanced') ON CONFLICT (id) DO UPDATE SET version = 6, note = 'advanced' WHERE conditional_upsert.version <= 6",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conditional_upsert VALUES (1, 4, 'stale') ON CONFLICT (id) DO UPDATE SET version = 4, note = 'stale' WHERE conditional_upsert.version < 4",
                    expected: Expected::Tag("INSERT 0 0"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conditional_upsert VALUES (1, 7, 'one'), (2, 0, 'two'), (3, 3, 'three') ON CONFLICT (id) DO UPDATE SET version = 7, note = 'updated' WHERE conditional_upsert.version <= 5",
                    expected: Expected::Tag("INSERT 0 2"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conditional_upsert VALUES (2, 8, 'null predicate') ON CONFLICT (id) DO UPDATE SET version = 8 WHERE NULL",
                    expected: Expected::Tag("INSERT 0 0"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conditional_upsert VALUES (2, $1, 'bound') ON CONFLICT (id) DO UPDATE SET version = $1, note = 'bound' WHERE conditional_upsert.version < $1",
                    bind_vars: &[BindVar::Int(8)],
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conditional_upsert VALUES (1, 9, 'proposed') ON CONFLICT (id) DO UPDATE SET version = excluded.version, note = excluded.note WHERE conditional_upsert.version < excluded.version RETURNING id, version, note",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("version", INT4), Column("note", TEXT)],
                        rows: &[
                            &[T("1"), T("9"), T("proposed")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conditional_upsert VALUES (1, 10, 'casted') ON CONFLICT (id) DO UPDATE SET version = excluded.version::BIGINT, note = excluded.note WHERE conditional_upsert.version < excluded.version RETURNING id, version",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("version", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conditional_upsert SELECT 1, 11::BIGINT, 'selected' ON CONFLICT (id) DO UPDATE SET version = excluded.version, note = excluded.note WHERE conditional_upsert.version < excluded.version RETURNING id, version, note",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("version", INT4), Column("note", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("selected")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM conditional_upsert ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("version", INT4), Column("note", TEXT)],
                        rows: &[
                            &[T("1"), T("11"), T("selected")],
                            &[T("2"), T("8"), T("bound")],
                            &[T("3"), T("3"), T("three")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "on conflict update returning with check constraint",
            set_up_script: &[
                "CREATE TABLE checked_upsert (id INT PRIMARY KEY, a TEXT CHECK (a <> ''), b TEXT, c TEXT)",
                "INSERT INTO checked_upsert VALUES (1, 'x', 'y', 'z')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO checked_upsert VALUES (1, 'x', 'y', 'z') ON CONFLICT (id) DO UPDATE SET a = 'n1', b = 'n2' RETURNING id, a, b",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", TEXT), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("n1"), T("n2")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO checked_upsert VALUES (2, 'x', 'y', 'z') ON CONFLICT (id) DO UPDATE SET a = 'n1', b = 'n2' RETURNING id, a, b",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", TEXT), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "on conflict do nothing only ignores uniqueness conflicts",
            set_up_script: &[
                "CREATE TABLE conflict_parent (id INT PRIMARY KEY)",
                "CREATE TABLE conflict_child (id INT PRIMARY KEY, parent_id INT NOT NULL REFERENCES conflict_parent(id), positive INT CHECK (positive > 0))",
                "CREATE TABLE self_referencing_child (id INT PRIMARY KEY, parent_id INT REFERENCES self_referencing_child(id))",
                "CREATE TABLE secondary_unique_child (id INT PRIMARY KEY, unique_value INT UNIQUE, parent_id INT REFERENCES conflict_parent(id))",
                "CREATE TABLE conflict_arbiter (id INT PRIMARY KEY, unique_value INT UNIQUE, a INT, b INT, UNIQUE (a, b))",
                "CREATE TABLE invalid_conflict_target (id INT PRIMARY KEY, non_unique INT)",
                "CREATE TABLE crossed_conflict (id INT PRIMARY KEY, unique_value INT UNIQUE)",
                "CREATE TABLE crossed_keyless_conflict (a INT UNIQUE, b INT UNIQUE)",
                "CREATE TABLE crossed_partial_conflict (a INT, b INT UNIQUE)",
                "CREATE UNIQUE INDEX a_partial ON crossed_partial_conflict (a) WHERE b > 0",
                "CREATE UNIQUE INDEX z_full ON crossed_partial_conflict (a)",
                "INSERT INTO conflict_parent VALUES (1)",
                "INSERT INTO conflict_child VALUES (1, 1, 1)",
                "INSERT INTO secondary_unique_child VALUES (1, 10, 1)",
                "INSERT INTO conflict_arbiter VALUES (1, 10, 20, 30)",
                "INSERT INTO crossed_conflict VALUES (1, 10), (2, 20)",
                "INSERT INTO crossed_keyless_conflict VALUES (1, 10), (2, 20)",
                "INSERT INTO crossed_partial_conflict VALUES (1, 10), (2, 20)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO conflict_child VALUES (2, 999, 1) ON CONFLICT DO NOTHING",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "conflict_child" violates foreign key constraint "conflict_child_parent_id_fkey""#, detail: r#"Key (parent_id)=(999) is not present in table "conflict_parent"."#, schema: "public", table: "conflict_child", constraint: "conflict_child_parent_id_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conflict_child VALUES (2, 1, -1) ON CONFLICT DO NOTHING",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "conflict_child" violates check constraint "conflict_child_positive_check""#, detail: "Failing row contains (2, 1, -1).", schema: "public", table: "conflict_child", constraint: "conflict_child_positive_check", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conflict_child VALUES (1, 999, 1) ON CONFLICT DO NOTHING",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO secondary_unique_child VALUES (2, 10, 999) ON CONFLICT DO NOTHING",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM secondary_unique_child ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("unique_value", INT4), Column("parent_id", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conflict_arbiter VALUES (1, 11, 21, 31) ON CONFLICT (id) DO NOTHING",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conflict_arbiter VALUES (2, 10, 21, 31) ON CONFLICT (id) DO NOTHING",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "conflict_arbiter_unique_value_key""#, detail: "Key (unique_value)=(10) already exists.", schema: "public", table: "conflict_arbiter", constraint: "conflict_arbiter_unique_value_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conflict_arbiter VALUES (2, 11, 20, 30) ON CONFLICT (a, b) DO NOTHING",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conflict_arbiter VALUES (2, 10, 21, 31) ON CONFLICT DO NOTHING",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM conflict_arbiter ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("unique_value", INT4), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("20"), T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO invalid_conflict_target VALUES (1, 10) ON CONFLICT (non_unique) DO NOTHING",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: "there is no unique or exclusion constraint matching the ON CONFLICT specification", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO invalid_conflict_target VALUES (1, 10) ON CONFLICT (missing) DO NOTHING",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "missing" does not exist"#, position: 64, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM invalid_conflict_target",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("non_unique", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO crossed_conflict VALUES (1, 20) ON CONFLICT (id) DO NOTHING",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO crossed_conflict VALUES (1, 20) ON CONFLICT (unique_value) DO NOTHING",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO crossed_conflict VALUES (3, 30), (3, 10) ON CONFLICT (id) DO NOTHING",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM crossed_conflict ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("unique_value", INT4)],
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
                    query: "INSERT INTO crossed_keyless_conflict VALUES (3, 30), (3, 10) ON CONFLICT (a) DO NOTHING",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM crossed_keyless_conflict ORDER BY a",
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
                    query: "INSERT INTO crossed_partial_conflict VALUES (3, -1), (3, 10) ON CONFLICT (a) DO NOTHING",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM crossed_partial_conflict ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                            &[T("3"), T("-1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO self_referencing_child VALUES (1, 1) ON CONFLICT DO NOTHING",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM self_referencing_child",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("parent_id", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conflict_child VALUES (1, 1, 1), (2, 1, 1) ON CONFLICT DO NOTHING RETURNING id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM conflict_child ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("parent_id", INT4), Column("positive", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO conflict_child VALUES (2, 1, 1), (3, 999, 1) ON CONFLICT DO NOTHING",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "conflict_child" violates foreign key constraint "conflict_child_parent_id_fkey""#, detail: r#"Key (parent_id)=(999) is not present in table "conflict_parent"."#, schema: "public", table: "conflict_child", constraint: "conflict_child_parent_id_fkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM conflict_child ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("parent_id", INT4), Column("positive", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "null and unspecified default values",
            set_up_script: &[
                "CREATE TABLE t (i INT DEFAULT NULL, j INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (default, default)",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4)],
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
            name: "default values compatibility",
            set_up_script: &[
                "CREATE TABLE ordinary_defaults (a INT DEFAULT 1, b INT DEFAULT 2)",
                "CREATE TABLE generated_defaults (a INT DEFAULT 1, b INT GENERATED ALWAYS AS (a + 1) STORED)",
                "CREATE TABLE identity_defaults (id BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, v INT DEFAULT 5)",
                "CREATE TEMP TABLE serial_defaults (small_id SMALLSERIAL, id SERIAL PRIMARY KEY, big_id BIGSERIAL, v INT DEFAULT 5)",
                "CREATE TABLE rejected_empty_rows (a INT DEFAULT 1, b INT DEFAULT 2)",
                "CREATE TABLE required_defaults (a INT NOT NULL, b INT DEFAULT 2)",
                "INSERT INTO required_defaults VALUES (9, 9)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO ordinary_defaults DEFAULT VALUES",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ordinary_defaults",
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
                    query: "INSERT INTO generated_defaults DEFAULT VALUES",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generated_defaults",
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
                    query: "INSERT INTO identity_defaults DEFAULT VALUES",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO identity_defaults VALUES (DEFAULT, DEFAULT), (DEFAULT, DEFAULT)",
                    expected: Expected::Tag("INSERT 0 2"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM identity_defaults ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("5")],
                            &[T("2"), T("5")],
                            &[T("3"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO serial_defaults DEFAULT VALUES",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO serial_defaults DEFAULT VALUES",
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM serial_defaults ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("small_id", INT2), Column("id", INT4), Column("big_id", INT8), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("5")],
                            &[T("2"), T("2"), T("2"), T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rejected_empty_rows VALUES ()",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near ")""#, position: 41, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rejected_empty_rows VALUES (), ()",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near ")""#, position: 41, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rejected_empty_rows () VALUES ()",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near ")""#, position: 34, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rejected_empty_rows (a) VALUES ()",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near ")""#, position: 45, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM rejected_empty_rows",
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
                    query: "INSERT INTO ordinary_defaults VALUES (DEFAULT, DEFAULT), (3, DEFAULT)",
                    expected: Expected::Tag("INSERT 0 2"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ordinary_defaults ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("1"), T("2")],
                            &[T("3"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO generated_defaults VALUES (DEFAULT, DEFAULT), (3, DEFAULT)",
                    expected: Expected::Tag("INSERT 0 2"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generated_defaults ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("1"), T("2")],
                            &[T("3"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO required_defaults VALUES (DEFAULT, DEFAULT), (5, DEFAULT)",
                    expected: Expected::Error(Diagnostic { code: "23502", message: r#"null value in column "a" of relation "required_defaults" violates not-null constraint"#, detail: "Failing row contains (null, 2).", schema: "public", table: "required_defaults", column: "a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM required_defaults",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("9"), T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "types",
            set_up_script: &[
                "create table child (i2 int2, i4 int4, i8 int8, f float, d double precision, v varchar, vl varchar(100), t text, j json, ts timestamp);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"insert into child values (1, 2, 3, 4.5, 6.7, 'hello', 'world', 'text', '{"a": 1}', '2021-01-01 00:00:00');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from child;",
                    expected: Expected::Rows {
                        columns: &[Column("i2", INT2), Column("i4", INT4), Column("i8", INT8), Column("f", FLOAT8), Column("d", FLOAT8), Column("v", VARCHAR), Column("vl", VARCHAR), Column("t", TEXT), Column("j", JSON), Column("ts", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("2"), T("3"), T("4.5"), T("6.7"), T("hello"), T("world"), T("text"), T(r#"{"a": 1}"#), T("2021-01-01 00:00:00")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert returning",
            set_up_script: &[
                "CREATE TABLE t (i serial, j INT)",
                "CREATE TABLE u (u uuid DEFAULT 'ac1f3e2d-1e4b-4d3e-8b1f-2b7f1e7f0e3d', j INT)",
                "CREATE TABLE s (v1 varchar DEFAULT 'hello', v2 varchar DEFAULT 'world')",
                "CREATE SCHEMA ts",
                "CREATE TABLE ts.t (i serial, j INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t (j) VALUES (5), (6), (7) RETURNING i",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "INSERT 0 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (j) VALUES (5), (6), (7) RETURNING i+3",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("7")],
                            &[T("8")],
                            &[T("9")],
                        ],
                        tag: "INSERT 0 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (j) VALUES (5), (6), (7) RETURNING i+j, j-3*i",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4), Column("?column?", INT4)],
                        rows: &[
                            &[T("12"), T("-16")],
                            &[T("14"), T("-18")],
                            &[T("16"), T("-20")],
                        ],
                        tag: "INSERT 0 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO u (j) VALUES (5), (6), (7) RETURNING u",
                    expected: Expected::Rows {
                        columns: &[Column("u", UUID)],
                        rows: &[
                            &[T("ac1f3e2d-1e4b-4d3e-8b1f-2b7f1e7f0e3d")],
                            &[T("ac1f3e2d-1e4b-4d3e-8b1f-2b7f1e7f0e3d")],
                            &[T("ac1f3e2d-1e4b-4d3e-8b1f-2b7f1e7f0e3d")],
                        ],
                        tag: "INSERT 0 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO s (v2) VALUES (' a') RETURNING concat(v1, v2)",
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T("hello a")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO s (v1) VALUES ('sup ') RETURNING concat(v1, v2)",
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT)],
                        rows: &[
                            &[T("sup world")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO s (v2, v1) VALUES ('def', 'abc'), ('xyz', 'uvw') RETURNING concat(v1, v2), concat(v2, v1), 100",
                    expected: Expected::Rows {
                        columns: &[Column("concat", TEXT), Column("concat", TEXT), Column("?column?", INT4)],
                        rows: &[
                            &[T("abcdef"), T("defabc"), T("100")],
                            &[T("uvwxyz"), T("xyzuvw"), T("100")],
                        ],
                        tag: "INSERT 0 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (j) VALUES (5), (6), (7) RETURNING i, doesnotexist",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "doesnotexist" does not exist"#, position: 53, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (j) VALUES (5), (6), (7) RETURNING i, doesnotexist(j)",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function doesnotexist(integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 53, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO public.t (j) VALUES (8) RETURNING t.j",
                    expected: Expected::Rows {
                        columns: &[Column("j", INT4)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO public.t (j) VALUES (9) RETURNING public.t.j",
                    expected: Expected::Rows {
                        columns: &[Column("j", INT4)],
                        rows: &[
                            &[T("9")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ts.t (j) VALUES (10) RETURNING ts.t.j",
                    expected: Expected::Rows {
                        columns: &[Column("j", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO public.t (j) VALUES ($1) RETURNING j;",
                    bind_vars: &[BindVar::Int(11)],
                    expected: Expected::Rows {
                        columns: &[Column("j", INT4)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO public.t (j) VALUES ($1) RETURNING t.j;",
                    bind_vars: &[BindVar::Int(12)],
                    expected: Expected::Rows {
                        columns: &[Column("j", INT4)],
                        rows: &[
                            &[T("12")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO public.t (j) VALUES ($1) RETURNING public.t.j;",
                    bind_vars: &[BindVar::Int(13)],
                    expected: Expected::Rows {
                        columns: &[Column("j", INT4)],
                        rows: &[
                            &[T("13")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert iso8601 timestamptz literal",
            set_up_script: &[
                "CREATE TABLE django_migrations (id serial primary key, app varchar, name varchar, applied timestamptz)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO "django_migrations" ("app", "name", "applied") VALUES ('contenttypes', '0001_initial', '2025-03-24T19:21:59.690479+00:00'::timestamptz) RETURNING "django_migrations"."id""#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert on conflict do nothing returning",
            set_up_script: &[
                "CREATE TABLE t4 (k INT PRIMARY KEY, v TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t4 VALUES (1, 'a') ON CONFLICT DO NOTHING RETURNING k, v;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t4 VALUES (1, 'b'), (2, 'c') ON CONFLICT DO NOTHING RETURNING k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t4 VALUES (1, 'b') ON CONFLICT (k) DO NOTHING RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "INSERT 0 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t4 ORDER BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("c")],
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
fn test_unique_statement_check_rules() {
    run_scripts(&[
        ScriptTest {
            name: "unique checks within one statement",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ust (a int, b int, c int UNIQUE, PRIMARY KEY (a, b));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ust SELECT x / 10, x % 10, x FROM generate_series(1, 1000) x;",
                    expected: Expected::Tag("INSERT 0 1000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), count(DISTINCT c) FROM ust;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("1000"), T("1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ust SELECT x / 10, x % 10, 5 FROM generate_series(2000, 2003) x;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "ust_c_key""#, detail: "Key (c)=(5) already exists.", schema: "public", table: "ust", constraint: "ust_c_key", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ust VALUES (500, 1, 2000), (500, 2, 2000);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "ust_c_key""#, detail: "Key (c)=(2000) already exists.", schema: "public", table: "ust", constraint: "ust_c_key", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ust VALUES (500, 1, NULL), (500, 2, NULL);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ust SET c = c + 5000 WHERE c > 990;",
                    expected: Expected::Tag("UPDATE 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT max(c) FROM ust;",
                    expected: Expected::Rows {
                        columns: &[Column("max", INT4)],
                        rows: &[
                            &[T("6000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM ust WHERE c = 5;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ust VALUES (600, 1, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "qualified function names in errors",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_catalog.nosuch(1);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function pg_catalog.nosuch(integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM unnest(ARRAY[1, 2], 5);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function pg_catalog.unnest(integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 15, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM unnest(ARRAY[1, 2], ARRAY['a']) AS t(a, b);",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), Null],
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
fn test_merge_statement() {
    run_scripts(&[
        ScriptTest {
            name: "MERGE",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE target (tid integer, balance integer);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE source (sid integer, delta integer);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO target VALUES (1, 10),(2, 20),(3, 30);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO source VALUES (2, 5),(3, 20),(4, 40);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO target t USING source AS s ON t.tid = s.sid WHEN MATCHED THEN UPDATE SET balance = t.balance + delta WHEN NOT MATCHED THEN INSERT (balance, tid) VALUES (balance + delta, sid);",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "balance" does not exist"#, detail: r#"There is a column named "balance" in table "t", but it cannot be referenced from this part of the query."#, position: 165, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO target t USING source AS s ON t.tid = s.sid WHEN NOT MATCHED THEN INSERT (balance, tid) VALUES (t.balance, sid);",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"invalid reference to FROM-clause entry for table "t""#, detail: r#"There is an entry for table "t", but it cannot be referenced from this part of the query."#, position: 108, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO target t USING source AS s ON t.tid = s.sid WHEN NOT MATCHED AND t.balance > 0 THEN INSERT (tid) VALUES (sid);",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"invalid reference to FROM-clause entry for table "t""#, detail: r#"There is an entry for table "t", but it cannot be referenced from this part of the query."#, position: 77, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO target t USING source AS s ON t.tid = s.sid WHEN MATCHED THEN UPDATE SET balance = balance + delta WHEN NOT MATCHED THEN INSERT VALUES (sid, delta);",
                    expected: Expected::Tag("MERGE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM target ORDER BY tid;",
                    expected: Expected::Rows {
                        columns: &[Column("tid", INT4), Column("balance", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("25")],
                            &[T("3"), T("50")],
                            &[T("4"), T("40")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO target t USING source AS s ON t.tid = s.sid WHEN MATCHED AND t.balance > 30 THEN DELETE WHEN MATCHED THEN DO NOTHING;",
                    expected: Expected::Tag("MERGE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM target ORDER BY tid;",
                    expected: Expected::Rows {
                        columns: &[Column("tid", INT4), Column("balance", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("25")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO source VALUES (2, 7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO target t USING source AS s ON t.tid = s.sid WHEN MATCHED THEN UPDATE SET balance = 0;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "MERGE command cannot affect row a second time", hint: "Ensure that not more than one source row matches any one target row.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO target t USING (SELECT 9 AS sid, 99 AS delta) s ON t.tid = s.sid WHEN NOT MATCHED AND s.delta > 50 THEN INSERT (balance, tid) VALUES (s.delta, s.sid);",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM target ORDER BY tid;",
                    expected: Expected::Rows {
                        columns: &[Column("tid", INT4), Column("balance", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("25")],
                            &[T("9"), T("99")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "MERGE errors",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE merge_t (tid INTEGER, balance INTEGER);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE merge_s (sid INTEGER, delta INTEGER);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO merge_t VALUES (1, 10);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO merge_s VALUES (1, 5), (1, 6), (4, 40);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO merge_t t USING merge_s s ON t.tid = s.sid WHEN MATCHED THEN UPDATE SET balance = 0;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "MERGE command cannot affect row a second time", hint: "Ensure that not more than one source row matches any one target row.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW merge_v AS SELECT * FROM merge_t;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO merge_v t USING merge_s s ON t.tid = s.sid WHEN MATCHED THEN DELETE;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "MERGE command cannot affect row a second time", hint: "Ensure that not more than one source row matches any one target row.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO merge_t t USING merge_s s ON t.tid = s.sid WHEN MATCHED THEN UPDATE SET nosuch = 1;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: r#"column "nosuch" of relation "merge_t" does not exist"#, position: 84, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO merge_t t USING merge_s s ON t.tid = s.sid WHEN NOT MATCHED THEN INSERT VALUES (1, 2, 3);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "INSERT has more expressions than target columns", position: 98, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO merge_t t USING merge_s s ON t.tid = s.sid WHEN NOT MATCHED THEN INSERT DEFAULT VALUES;",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM merge_t ORDER BY tid;",
                    expected: Expected::Rows {
                        columns: &[Column("tid", INT4), Column("balance", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "MERGE into views",
            set_up_script: &[
                "CREATE TABLE mt (id INT PRIMARY KEY, v INT);",
                "INSERT INTO mt VALUES (1, 10), (2, 20), (5, 50);",
                "CREATE TABLE ms (id INT, d INT);",
                "INSERT INTO ms VALUES (1, 1), (6, 6), (7, 7);",
                "CREATE VIEW mv AS SELECT id, v AS val, v * 2 AS dbl FROM mt WHERE id < 5;",
                "CREATE VIEW mvc AS SELECT * FROM mt WHERE v < 100 WITH CHECK OPTION;",
                "CREATE VIEW mg AS SELECT id, count(*) FROM mt GROUP BY id;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "MERGE INTO mv t USING ms s ON t.id = s.id WHEN MATCHED THEN UPDATE SET val = t.val + s.d WHEN NOT MATCHED THEN INSERT (id, val) VALUES (s.id, s.d);",
                    expected: Expected::Tag("MERGE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("2"), T("20")],
                            &[T("5"), T("50")],
                            &[T("6"), T("6")],
                            &[T("7"), T("7")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO mv t USING ms s ON t.id = s.id WHEN MATCHED THEN UPDATE SET dbl = 1;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"cannot merge into column "dbl" of view "mv""#, detail: r#"View columns that are not columns of their base relation are not updatable."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO mvc t USING (VALUES (2, 500)) s(id, v) ON t.id = s.id WHEN MATCHED THEN UPDATE SET v = s.v;",
                    expected: Expected::Error(Diagnostic { code: "44000", message: r#"new row violates check option for view "mvc""#, detail: r#"Failing row contains (2, 500)."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO mvc t USING (VALUES (9, 900)) s(id, v) ON t.id = s.id WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.v);",
                    expected: Expected::Error(Diagnostic { code: "44000", message: r#"new row violates check option for view "mvc""#, detail: r#"Failing row contains (9, 900)."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO mv t USING (VALUES (2), (5)) s(id) ON t.id = s.id WHEN MATCHED THEN DELETE;",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("5"), T("50")],
                            &[T("6"), T("6")],
                            &[T("7"), T("7")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO mg t USING ms s ON t.id = s.id WHEN NOT MATCHED THEN INSERT VALUES (s.id, 1);",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"cannot insert into view "mg""#, detail: r#"Views containing GROUP BY are not automatically updatable."#, hint: r#"To enable inserting into the view using MERGE, provide an INSTEAD OF INSERT trigger."#, ..E }),
                    ..A
                },
            ],
            ..S
        },    ]);
}

#[test]
fn test_insert_into_array_elements() {
    run_scripts(&[
        ScriptTest {
            name: "INSERT targets that name array elements and slices",
            set_up_script: &[
                "CREATE TABLE ia_t (pk INT PRIMARY KEY, f1 INT[]);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO ia_t (pk, f1[1:2]) VALUES (1, '{}');",
                    expected: Expected::Error(Diagnostic { code: "2202E", message: "source array too small", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ia_t (pk, f1[0:2147483647]) VALUES (2, '{}');",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "array size exceeds the maximum allowed (134217727)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ia_t (pk, f1[1]) VALUES (3, 7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ia_t (pk, f1[1:2]) VALUES (4, '{5,6}');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ia_t (pk, f1[1:2]) SELECT 5, '{5,6}';",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, f1 FROM ia_t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("f1", INT4_ARRAY)],
                        rows: &[
                            &[T("3"), T("{7}")],
                            &[T("4"), T("{5,6}")],
                            &[T("5"), T("{5,6}")],
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
fn test_on_conflict_arbiter_inference() {
    run_scripts(&[
        ScriptTest {
            name: "ON CONFLICT arbiters inferred from expressions, partial index predicates, and column sets",
            set_up_script: &[
                "CREATE TABLE oc (id INT PRIMARY KEY, a INT, b INT, t TEXT);",
                "CREATE UNIQUE INDEX oc_lower ON oc (lower(t));",
                "CREATE UNIQUE INDEX oc_part ON oc (a) WHERE b > 0;",
                "CREATE UNIQUE INDEX oc_ab ON oc (a, b);",
                "CREATE UNIQUE INDEX oc_ba ON oc (b, a);",
                "INSERT INTO oc VALUES (1, 1, 1, 'X');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (2, 2, 2, 'x') ON CONFLICT (lower(t)) DO UPDATE SET b = 10 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("10"), T("X")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (3, 1, 5, 'y') ON CONFLICT (a) WHERE b > 0 DO UPDATE SET t = 'z' RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("10"), T("z")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (4, 1, 5, 'w') ON CONFLICT (a) WHERE b > 1 DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (5, 1, 5, 'v') ON CONFLICT (a) DO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: "there is no unique or exclusion constraint matching the ON CONFLICT specification", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (6, 1, 10, 'u') ON CONFLICT (a, b) DO UPDATE SET t = 'ab' RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("10"), T("ab")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (7, 7, 7, 'q') ON CONFLICT (t) DO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: "there is no unique or exclusion constraint matching the ON CONFLICT specification", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (8, 1, 10, 'r') ON CONFLICT (lower(t) COLLATE \"C\") DO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: "there is no unique or exclusion constraint matching the ON CONFLICT specification", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO oc VALUES (9, 1, 10, 'U') ON CONFLICT (a, b) WHERE b > 0 DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM oc ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("10"), T("ab")],
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
fn test_identity_columns() {
    run_scripts(&[
        ScriptTest {
            name: "identity columns: GENERATED ALWAYS takes the sequence's values and refuses others unless OVERRIDING SYSTEM VALUE",
            set_up_script: &[
                "CREATE TABLE a (id INT GENERATED ALWAYS AS IDENTITY, v TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO a (v) VALUES ('x'), ('y');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO a VALUES (DEFAULT, 'z');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO a VALUES (10, 'bad');",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: r#"cannot insert a non-DEFAULT value into column "id""#, detail: r#"Column "id" is an identity column defined as GENERATED ALWAYS."#, hint: "Use OVERRIDING SYSTEM VALUE to override.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO a OVERRIDING SYSTEM VALUE VALUES (10, 'ok');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO a OVERRIDING USER VALUE VALUES (20, 'user');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO a (v) VALUES ('after') RETURNING id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM a ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                            &[T("4"), T("user")],
                            &[T("5"), T("after")],
                            &[T("10"), T("ok")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE a SET id = 5 WHERE v = 'x';",
                    expected: Expected::Error(Diagnostic { code: "428C9", message: r#"column "id" can only be updated to DEFAULT"#, detail: r#"Column "id" is an identity column defined as GENERATED ALWAYS."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE a SET id = DEFAULT WHERE v = 'x' RETURNING id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY a (v) FROM STDIN;",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "identity-values.tsv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY a (id, v) FROM STDIN;",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "identity-ids-values.tsv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO a (v) SELECT v FROM a WHERE id < 3;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM a ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                            &[T("4"), T("user")],
                            &[T("5"), T("after")],
                            &[T("6"), T("x")],
                            &[T("7"), T("n")],
                            &[T("8"), T("o")],
                            &[T("9"), T("y")],
                            &[T("10"), T("ok")],
                            &[T("50"), T("copied")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "identity columns: GENERATED BY DEFAULT takes a given value, and OVERRIDING USER VALUE replaces it",
            set_up_script: &[
                "CREATE TABLE b (id BIGINT GENERATED BY DEFAULT AS IDENTITY (START WITH 100 INCREMENT BY 10), v TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO b (v) VALUES ('p');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b VALUES (7, 'q');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b OVERRIDING USER VALUE VALUES (8, 'r');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b OVERRIDING SYSTEM VALUE VALUES (9, 's');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY b (v) FROM STDIN;",
                    expected: Expected::Tag("COPY 2"),
                    copy_from_stdin_file: "identity-values.tsv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY b (id, v) FROM STDIN;",
                    expected: Expected::Tag("COPY 1"),
                    copy_from_stdin_file: "identity-ids-values.tsv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE ins(TEXT) AS INSERT INTO b (v) VALUES ($1) RETURNING id;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE ins('e1');",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
                        rows: &[
                            &[T("140")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE ins('e2');",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8)],
                        rows: &[
                            &[T("150")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM b ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("v", TEXT)],
                        rows: &[
                            &[T("7"), T("q")],
                            &[T("9"), T("s")],
                            &[T("50"), T("copied")],
                            &[T("100"), T("p")],
                            &[T("110"), T("r")],
                            &[T("120"), T("n")],
                            &[T("130"), T("o")],
                            &[T("140"), T("e1")],
                            &[T("150"), T("e2")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "identity columns: SMALLINT keys, a primary key, and a sequence that runs out",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE c (id SMALLINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, v TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c (v) VALUES ('m') RETURNING id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT2)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c VALUES (2, 'taken');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c (v) VALUES ('n');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "c_pkey""#, detail: "Key (id)=(2) already exists.", schema: "public", table: "c", constraint: "c_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO c (v) VALUES ('o') RETURNING id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT2)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE d (id INT GENERATED ALWAYS AS IDENTITY (MAXVALUE 2), v TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d (v) VALUES ('1'), ('2'), ('3');",
                    expected: Expected::Error(Diagnostic { code: "2200H", message: r#"nextval: reached maximum value of sequence "d_id_seq" (2)"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d (v) VALUES ('1'), ('2');",
                    expected: Expected::Error(Diagnostic { code: "2200H", message: r#"nextval: reached maximum value of sequence "d_id_seq" (2)"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO d (v) VALUES ('3');",
                    expected: Expected::Error(Diagnostic { code: "2200H", message: r#"nextval: reached maximum value of sequence "d_id_seq" (2)"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM d ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
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
fn test_unique_checks_pass_row_lockers() {
    run_scripts(&[
        ScriptTest {
            name: "unique checks: a duplicate of a row another transaction locks fails at once",
            set_up_script: &[
                "CREATE TABLE lockdup (k INT PRIMARY KEY, u INT UNIQUE);",
                "INSERT INTO lockdup VALUES (1, 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k FROM lockdup WHERE k = 1 FOR SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lockdup VALUES (1, 11);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "lockdup_pkey""#, detail: "Key (k)=(1) already exists.", schema: "public", table: "lockdup", constraint: "lockdup_pkey", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lockdup VALUES (2, 10);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "lockdup_u_key""#, detail: "Key (u)=(10) already exists.", schema: "public", table: "lockdup", constraint: "lockdup_u_key", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lockdup VALUES (1, 12) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    client: "B",
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
fn test_on_conflict() {
    run_scripts(&[
        ScriptTest {
            name: "ON CONFLICT DO NOTHING and DO UPDATE through primary key, unique, and constraint arbiters",
            set_up_script: &[
                "CREATE TABLE up (id INT PRIMARY KEY, v TEXT, u INT UNIQUE, n INT DEFAULT 0);",
                "INSERT INTO up VALUES (1, 'one', 10), (2, 'two', 20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (1, 'uno', 11) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (3, 'three', 10) ON CONFLICT DO NOTHING RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4), Column("n", INT4)],
                        rows: &[
                        ],
                        tag: "INSERT 0 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (3, 'three', 30), (3, 'tres', 31) ON CONFLICT (id) DO NOTHING RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("3"), T("three"), T("30"), T("0")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (1, 'uno', 11) ON CONFLICT (id) DO UPDATE SET v = excluded.v, n = up.n + 1 RETURNING old.*, new.*;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4), Column("n", INT4), Column("id", INT4), Column("v", TEXT), Column("u", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("1"), T("one"), T("10"), T("0"), T("1"), T("uno"), T("10"), T("1")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (4, 'four', 20) ON CONFLICT (u) DO UPDATE SET v = excluded.v || '!', n = up.n + 1 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("2"), T("four!"), T("20"), T("1")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (5, 'five', 50), (5, 'cinco', 51) ON CONFLICT (id) DO UPDATE SET v = excluded.v;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "ON CONFLICT DO UPDATE command cannot affect row a second time", hint: "Ensure that no rows proposed for insertion within the same command have duplicate constrained values.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (1, 'x', 0), (1, 'y', 0) ON CONFLICT (id) DO UPDATE SET v = excluded.v;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "ON CONFLICT DO UPDATE command cannot affect row a second time", hint: "Ensure that no rows proposed for insertion within the same command have duplicate constrained values.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (1, 'skipped', 0) ON CONFLICT (id) DO UPDATE SET v = excluded.v WHERE up.v = 'nope' RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4), Column("n", INT4)],
                        rows: &[
                        ],
                        tag: "INSERT 0 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (1, 'kept', 0) ON CONFLICT (id) DO UPDATE SET v = excluded.v WHERE up.v = 'uno' RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("1"), T("kept"), T("10"), T("1")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (2, 'dos', 0) ON CONFLICT ON CONSTRAINT up_pkey DO UPDATE SET v = excluded.v RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("2"), T("dos"), T("20"), T("1")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (6, 'six', 30) ON CONFLICT ON CONSTRAINT up_u_key DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (1, 'clash', 0) ON CONFLICT (id) DO UPDATE SET u = 30;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"up_u_key\"", detail: "Key (u)=(30) already exists.", schema: "public", table: "up", constraint: "up_u_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (2, 'moved key', 0) ON CONFLICT (id) DO UPDATE SET id = 20 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("20"), T("dos"), T("20"), T("1")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO up VALUES (7, 'seven', 70) ON CONFLICT (id) DO UPDATE SET v = 'never' RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("7"), T("seven"), T("70"), T("0")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM up ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("1"), T("kept"), T("10"), T("1")],
                            &[T("3"), T("three"), T("30"), T("0")],
                            &[T("7"), T("seven"), T("70"), T("0")],
                            &[T("20"), T("dos"), T("20"), T("1")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ON CONFLICT DO UPDATE fires the INSERT and UPDATE triggers of each path",
            set_up_script: &[
                "CREATE TABLE tr (id INT PRIMARY KEY, v TEXT);",
                "INSERT INTO tr VALUES (1, 'one');",
                "CREATE FUNCTION tr_note() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE NOTICE '% % % %', TG_WHEN, TG_LEVEL, TG_OP, CASE WHEN TG_LEVEL = 'ROW' AND TG_OP <> 'DELETE' THEN NEW.v ELSE '' END; IF TG_LEVEL = 'ROW' THEN RETURN NEW; END IF; RETURN NULL; END $$;",
                "CREATE TRIGGER tr_bs_i BEFORE INSERT ON tr FOR EACH STATEMENT EXECUTE FUNCTION tr_note();",
                "CREATE TRIGGER tr_bs_u BEFORE UPDATE ON tr FOR EACH STATEMENT EXECUTE FUNCTION tr_note();",
                "CREATE TRIGGER tr_as_i AFTER INSERT ON tr FOR EACH STATEMENT EXECUTE FUNCTION tr_note();",
                "CREATE TRIGGER tr_as_u AFTER UPDATE ON tr FOR EACH STATEMENT EXECUTE FUNCTION tr_note();",
                "CREATE TRIGGER tr_br_i BEFORE INSERT ON tr FOR EACH ROW EXECUTE FUNCTION tr_note();",
                "CREATE TRIGGER tr_br_u BEFORE UPDATE ON tr FOR EACH ROW EXECUTE FUNCTION tr_note();",
                "CREATE TRIGGER tr_ar_i AFTER INSERT ON tr FOR EACH ROW EXECUTE FUNCTION tr_note();",
                "CREATE TRIGGER tr_ar_u AFTER UPDATE ON tr FOR EACH ROW EXECUTE FUNCTION tr_note();",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO tr VALUES (1, 'uno'), (2, 'two') ON CONFLICT (id) DO UPDATE SET v = excluded.v || ' updated';",
                    notices: &[Diagnostic { code: "00000", message: "BEFORE STATEMENT INSERT ", ..N }, Diagnostic { code: "00000", message: "BEFORE STATEMENT UPDATE ", ..N }, Diagnostic { code: "00000", message: "BEFORE ROW INSERT uno", ..N }, Diagnostic { code: "00000", message: "BEFORE ROW UPDATE uno updated", ..N }, Diagnostic { code: "00000", message: "BEFORE ROW INSERT two", ..N }, Diagnostic { code: "00000", message: "AFTER ROW UPDATE uno updated", ..N }, Diagnostic { code: "00000", message: "AFTER ROW INSERT two", ..N }, Diagnostic { code: "00000", message: "AFTER STATEMENT UPDATE ", ..N }, Diagnostic { code: "00000", message: "AFTER STATEMENT INSERT ", ..N }],
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tr VALUES (1, 'nothing') ON CONFLICT (id) DO NOTHING;",
                    notices: &[Diagnostic { code: "00000", message: "BEFORE STATEMENT INSERT ", ..N }, Diagnostic { code: "00000", message: "BEFORE ROW INSERT nothing", ..N }, Diagnostic { code: "00000", message: "AFTER STATEMENT INSERT ", ..N }],
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM tr ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("uno updated")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ON CONFLICT on a partitioned table maps its arbiters and DO UPDATE to each partition",
            set_up_script: &[
                "CREATE TABLE pu (a INT, b TEXT, c INT) PARTITION BY LIST (a);",
                "CREATE TABLE pu1 (c INT, b TEXT, a INT);",
                "ALTER TABLE pu ATTACH PARTITION pu1 FOR VALUES IN (1, 2);",
                "CREATE TABLE pu3 PARTITION OF pu FOR VALUES IN (3);",
                "CREATE UNIQUE INDEX pu_a ON pu (a);",
                "INSERT INTO pu VALUES (1, 'one', 10), (3, 'three', 30);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO pu VALUES (1, 'uno', 11), (2, 'two', 20), (3, 'tres', 31) ON CONFLICT (a) DO UPDATE SET b = excluded.b, c = pu.c + excluded.c WHERE pu.c < 20 RETURNING tableoid::regclass, old.*, new.*;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT), Column("c", INT4), Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("pu1"), T("1"), T("one"), T("10"), T("1"), T("uno"), T("21")],
                            &[T("pu1"), Null, Null, Null, T("2"), T("two"), T("20")],
                        ],
                        tag: "INSERT 0 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pu VALUES (2, 'dos', 0) ON CONFLICT (a) DO UPDATE SET a = 3;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "invalid ON UPDATE specification", detail: "The result tuple would appear in a different partition than the original tuple.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pu VALUES (2, 'dos', 0) ON CONFLICT (a) DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) INSERT INTO pu VALUES (1, 'x', 0) ON CONFLICT (a) DO UPDATE SET b = excluded.b WHERE pu.b <> excluded.b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Insert on pu")],
                            &[T("  Conflict Resolution: UPDATE")],
                            &[T("  Conflict Arbiter Indexes: pu_a")],
                            &[T("  Conflict Filter: (pu.b <> excluded.b)")],
                            &[T("  ->  Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM pu ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("a", INT4), Column("b", TEXT), Column("c", INT4)],
                        rows: &[
                            &[T("pu1"), T("1"), T("uno"), T("21")],
                            &[T("pu1"), T("2"), T("two"), T("20")],
                            &[T("pu3"), T("3"), T("three"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ON CONFLICT through a view, a table without a primary key, deferrable constraints, and an index that is not valid",
            set_up_script: &[
                "CREATE TABLE vb (id INT PRIMARY KEY, v INT, g INT GENERATED ALWAYS AS (v * 2) STORED);",
                "INSERT INTO vb VALUES (1, 1), (2, 5);",
                "CREATE VIEW vv AS SELECT id, v FROM vb WHERE v < 10 WITH CHECK OPTION;",
                "CREATE TABLE kl (a INT, b INT);",
                "CREATE UNIQUE INDEX kl_a ON kl (a);",
                "CREATE UNIQUE INDEX kl_b ON kl (b) NULLS NOT DISTINCT;",
                "CREATE TABLE dfr (a INT UNIQUE DEFERRABLE, b INT UNIQUE);",
                "CREATE TABLE pinv (a INT, b INT) PARTITION BY RANGE (a);",
                "CREATE TABLE pinv1 PARTITION OF pinv FOR VALUES FROM (0) TO (10);",
                "CREATE UNIQUE INDEX pinv_a ON ONLY pinv (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO vv VALUES (1, 3) ON CONFLICT (id) DO UPDATE SET v = excluded.v + vv.v RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("4")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO vv VALUES (2, 7) ON CONFLICT (id) DO UPDATE SET v = excluded.v + vv.v;",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"vv\"", detail: "Failing row contains (2, 12, 24).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO vv VALUES (3, 30) ON CONFLICT (id) DO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "44000", message: "new row violates check option for view \"vv\"", detail: "Failing row contains (3, 30, 60).", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vb ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("4"), T("8")],
                            &[T("2"), T("5"), T("10")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kl VALUES (1, 1), (NULL, 2), (NULL, 3) ON CONFLICT (a) DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kl VALUES (1, 9) ON CONFLICT (a) DO UPDATE SET b = kl.b + 100 RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("101")],
                        ],
                        tag: "INSERT 0 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kl VALUES (5, NULL), (6, NULL) ON CONFLICT (b) DO UPDATE SET a = excluded.a RETURNING *;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "ON CONFLICT DO UPDATE command cannot affect row a second time", hint: "Ensure that no rows proposed for insertion within the same command have duplicate constrained values.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kl VALUES (2, 101) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kl VALUES (7, 101) ON CONFLICT (a) DO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"kl_b\"", detail: "Key (b)=(101) already exists.", schema: "public", table: "kl", constraint: "kl_b", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM kl ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("101")],
                            &[Null, T("2")],
                            &[Null, T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dfr VALUES (1, 1) ON CONFLICT DO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "ON CONFLICT does not support deferrable unique constraints/exclusion constraints as arbiters", schema: "public", table: "dfr", constraint: "dfr_a_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dfr VALUES (1, 1) ON CONFLICT (a) DO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "ON CONFLICT does not support deferrable unique constraints/exclusion constraints as arbiters", schema: "public", table: "dfr", constraint: "dfr_a_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dfr VALUES (1, 1) ON CONFLICT (b) DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indisvalid FROM pg_index WHERE indexrelid = 'pinv_a'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indisvalid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pinv VALUES (1, 1) ON CONFLICT (a) DO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: "there is no unique or exclusion constraint matching the ON CONFLICT specification", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pinv VALUES (1, 1) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "A unique index CREATE INDEX CONCURRENTLY left not valid is no arbiter",
            set_up_script: &[
                "CREATE TABLE ci (a INT, b INT);",
                "INSERT INTO ci VALUES (1, 1), (1, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX CONCURRENTLY ci_a ON ci (a);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"ci_a\"", detail: "Key (a)=(1) is duplicated.", schema: "public", table: "ci", constraint: "ci_a", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass, indisvalid, indisready FROM pg_index WHERE indrelid = 'ci'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", REGCLASS), Column("indisvalid", BOOL), Column("indisready", BOOL)],
                        rows: &[
                            &[T("ci_a"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ci VALUES (1, 3) ON CONFLICT (a) DO NOTHING;",
                    expected: Expected::Error(Diagnostic { code: "42P10", message: "there is no unique or exclusion constraint matching the ON CONFLICT specification", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ci VALUES (1, 3) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ci;",
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
        ScriptTest {
            name: "Concurrent ON CONFLICT waits for the writer of the conflicting row and acts on its outcome",
            set_up_script: &[
                "CREATE TABLE cc (id INT PRIMARY KEY, v TEXT, u INT UNIQUE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO cc VALUES (1, 'a', 10)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO cc VALUES (1, 'b', 11) ON CONFLICT (id) DO NOTHING",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM cc ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("a"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO cc VALUES (2, 'a', 20)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO cc VALUES (2, 'b', 21) ON CONFLICT (id) DO UPDATE SET v = excluded.v",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM cc ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("a"), T("10")],
                            &[T("2"), T("b"), T("21")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO cc VALUES (3, 'a', 30)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO cc VALUES (4, 'b', 30) ON CONFLICT (u) DO UPDATE SET v = excluded.v",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM cc ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("a"), T("10")],
                            &[T("2"), T("b"), T("21")],
                            &[T("3"), T("b"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE cc SET v = 'a2' WHERE id = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO cc VALUES (1, 'b', 0) ON CONFLICT (id) DO UPDATE SET v = cc.v || '+' || excluded.v",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM cc ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("a2+b"), T("10")],
                            &[T("2"), T("b"), T("21")],
                            &[T("3"), T("b"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DELETE FROM cc WHERE id = 2",
                    expected: Expected::Tag("DELETE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO cc VALUES (2, 'b', 22) ON CONFLICT (id) DO UPDATE SET v = 'never'",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM cc ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("u", INT4)],
                        rows: &[
                            &[T("1"), T("a2+b"), T("10")],
                            &[T("2"), T("b"), T("22")],
                            &[T("3"), T("b"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM cc",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO cc VALUES (5, 'a', 50)",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO cc VALUES (5, 'b', 51) ON CONFLICT DO NOTHING",
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
                    query: "/* client B */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM cc",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE cc SET v = 'z' WHERE id = 5",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO cc VALUES (5, 'b', 51) ON CONFLICT (id) DO UPDATE SET v = excluded.v",
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
                    query: "/* client B */ BEGIN ISOLATION LEVEL REPEATABLE READ",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT count(*) FROM cc",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO cc VALUES (6, 'b', 60), (6, 'c', 61) ON CONFLICT DO NOTHING",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Two upserts that each wait for the other's inserted row deadlock",
            set_up_script: &[
                "CREATE TABLE dl (id INT PRIMARY KEY, v TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO dl VALUES (1, 'a')",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO dl VALUES (2, 'b')",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO dl VALUES (2, 'a') ON CONFLICT (id) DO UPDATE SET v = excluded.v",
                    expected: Expected::Error(Diagnostic { code: "40P01", message: "deadlock detected", hint: "See server log for query details.", ..E }),
                    client: "A",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO dl VALUES (1, 'b') ON CONFLICT (id) DO UPDATE SET v = excluded.v",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT * FROM dl ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("b")],
                            &[T("2"), T("b")],
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
fn test_on_conflict_with_concurrent_partition_moves() {
    run_scripts(&[
        ScriptTest {
            name: "ON CONFLICT DO UPDATE inserts the row when a concurrent update moved the conflicting row to another partition",
            set_up_script: &[
                "CREATE TABLE ocp (i int PRIMARY KEY, j int, k int) PARTITION BY RANGE (i)",
                "CREATE TABLE ocp_1 PARTITION OF ocp FOR VALUES FROM (1) TO (100)",
                "CREATE TABLE ocp_2 PARTITION OF ocp FOR VALUES FROM (100) TO (200)",
                "INSERT INTO ocp VALUES (1, 10, 100)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ SELECT i, j, k FROM ocp WHERE i = 1 FOR UPDATE",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
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
                    query: "/* client B */ INSERT INTO ocp VALUES (1, 11, 111) ON CONFLICT (i) DO UPDATE SET k = EXCLUDED.k",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE ocp SET i = i + 150 WHERE i = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT i, j, k FROM ocp ORDER BY i",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("j", INT4), Column("k", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("111")],
                            &[T("151"), T("10"), T("100")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ COMMIT",
                    expected: Expected::Tag("COMMIT"),
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_rows_too_big() {
    run_scripts(&[
        ScriptTest {
            name: "rows longer than Postgres stores, counted as Postgres counts a heap row once its movable values are out of line",
            set_up_script: &[
                "CREATE TABLE p (id INT PRIMARY KEY, t TEXT STORAGE PLAIN);",
                "CREATE TABLE e (id INT PRIMARY KEY, t TEXT, m TEXT STORAGE MAIN, x TEXT STORAGE EXTERNAL);",
                "CREATE TABLE w (c0 INT8, c1 INT8, c2 INT8, c3 INT8, c4 INT8, c5 INT8, c6 INT8, c7 INT8, c8 INT8, c9 INT8, c10 INT8, c11 INT8, c12 INT8, c13 INT8, c14 INT8, c15 INT8, c16 INT8, c17 INT8, c18 INT8, c19 INT8, c20 INT8, c21 INT8, c22 INT8, c23 INT8, c24 INT8, c25 INT8, c26 INT8, c27 INT8, c28 INT8, c29 INT8, c30 INT8, c31 INT8, c32 INT8, c33 INT8, c34 INT8, c35 INT8, c36 INT8, c37 INT8, c38 INT8, c39 INT8, c40 INT8, c41 INT8, c42 INT8, c43 INT8, c44 INT8, c45 INT8, c46 INT8, c47 INT8, c48 INT8, c49 INT8, c50 INT8, c51 INT8, c52 INT8, c53 INT8, c54 INT8, c55 INT8, c56 INT8, c57 INT8, c58 INT8, c59 INT8, c60 INT8, c61 INT8, c62 INT8, c63 INT8, c64 INT8, c65 INT8, c66 INT8, c67 INT8, c68 INT8, c69 INT8, c70 INT8, c71 INT8, c72 INT8, c73 INT8, c74 INT8, c75 INT8, c76 INT8, c77 INT8, c78 INT8, c79 INT8, c80 INT8, c81 INT8, c82 INT8, c83 INT8, c84 INT8, c85 INT8, c86 INT8, c87 INT8, c88 INT8, c89 INT8, c90 INT8, c91 INT8, c92 INT8, c93 INT8, c94 INT8, c95 INT8, c96 INT8, c97 INT8, c98 INT8, c99 INT8, c100 INT8, c101 INT8, c102 INT8, c103 INT8, c104 INT8, c105 INT8, c106 INT8, c107 INT8, c108 INT8, c109 INT8, c110 INT8, c111 INT8, c112 INT8, c113 INT8, c114 INT8, c115 INT8, c116 INT8, c117 INT8, c118 INT8, c119 INT8, c120 INT8, c121 INT8, c122 INT8, c123 INT8, c124 INT8, c125 INT8, c126 INT8, c127 INT8, c128 INT8, c129 INT8, c130 INT8, c131 INT8, c132 INT8, c133 INT8, c134 INT8, c135 INT8, c136 INT8, c137 INT8, c138 INT8, c139 INT8, c140 INT8, c141 INT8, c142 INT8, c143 INT8, c144 INT8, c145 INT8, c146 INT8, c147 INT8, c148 INT8, c149 INT8, c150 INT8, c151 INT8, c152 INT8, c153 INT8, c154 INT8, c155 INT8, c156 INT8, c157 INT8, c158 INT8, c159 INT8, c160 INT8, c161 INT8, c162 INT8, c163 INT8, c164 INT8, c165 INT8, c166 INT8, c167 INT8, c168 INT8, c169 INT8, c170 INT8, c171 INT8, c172 INT8, c173 INT8, c174 INT8, c175 INT8, c176 INT8, c177 INT8, c178 INT8, c179 INT8, c180 INT8, c181 INT8, c182 INT8, c183 INT8, c184 INT8, c185 INT8, c186 INT8, c187 INT8, c188 INT8, c189 INT8, c190 INT8, c191 INT8, c192 INT8, c193 INT8, c194 INT8, c195 INT8, c196 INT8, c197 INT8, c198 INT8, c199 INT8, c200 INT8, c201 INT8, c202 INT8, c203 INT8, c204 INT8, c205 INT8, c206 INT8, c207 INT8, c208 INT8, c209 INT8, c210 INT8, c211 INT8, c212 INT8, c213 INT8, c214 INT8, c215 INT8, c216 INT8, c217 INT8, c218 INT8, c219 INT8, c220 INT8, c221 INT8, c222 INT8, c223 INT8, c224 INT8, c225 INT8, c226 INT8, c227 INT8, c228 INT8, c229 INT8, c230 INT8, c231 INT8, c232 INT8, c233 INT8, c234 INT8, c235 INT8, c236 INT8, c237 INT8, c238 INT8, c239 INT8, c240 INT8, c241 INT8, c242 INT8, c243 INT8, c244 INT8, c245 INT8, c246 INT8, c247 INT8, c248 INT8, c249 INT8, c250 INT8, c251 INT8, c252 INT8, c253 INT8, c254 INT8, c255 INT8, c256 INT8, c257 INT8, c258 INT8, c259 INT8, c260 INT8, c261 INT8, c262 INT8, c263 INT8, c264 INT8, c265 INT8, c266 INT8, c267 INT8, c268 INT8, c269 INT8, c270 INT8, c271 INT8, c272 INT8, c273 INT8, c274 INT8, c275 INT8, c276 INT8, c277 INT8, c278 INT8, c279 INT8, c280 INT8, c281 INT8, c282 INT8, c283 INT8, c284 INT8, c285 INT8, c286 INT8, c287 INT8, c288 INT8, c289 INT8, c290 INT8, c291 INT8, c292 INT8, c293 INT8, c294 INT8, c295 INT8, c296 INT8, c297 INT8, c298 INT8, c299 INT8, c300 INT8, c301 INT8, c302 INT8, c303 INT8, c304 INT8, c305 INT8, c306 INT8, c307 INT8, c308 INT8, c309 INT8, c310 INT8, c311 INT8, c312 INT8, c313 INT8, c314 INT8, c315 INT8, c316 INT8, c317 INT8, c318 INT8, c319 INT8, c320 INT8, c321 INT8, c322 INT8, c323 INT8, c324 INT8, c325 INT8, c326 INT8, c327 INT8, c328 INT8, c329 INT8, c330 INT8, c331 INT8, c332 INT8, c333 INT8, c334 INT8, c335 INT8, c336 INT8, c337 INT8, c338 INT8, c339 INT8, c340 INT8, c341 INT8, c342 INT8, c343 INT8, c344 INT8, c345 INT8, c346 INT8, c347 INT8, c348 INT8, c349 INT8, c350 INT8, c351 INT8, c352 INT8, c353 INT8, c354 INT8, c355 INT8, c356 INT8, c357 INT8, c358 INT8, c359 INT8, c360 INT8, c361 INT8, c362 INT8, c363 INT8, c364 INT8, c365 INT8, c366 INT8, c367 INT8, c368 INT8, c369 INT8, c370 INT8, c371 INT8, c372 INT8, c373 INT8, c374 INT8, c375 INT8, c376 INT8, c377 INT8, c378 INT8, c379 INT8, c380 INT8, c381 INT8, c382 INT8, c383 INT8, c384 INT8, c385 INT8, c386 INT8, c387 INT8, c388 INT8, c389 INT8, c390 INT8, c391 INT8, c392 INT8, c393 INT8, c394 INT8, c395 INT8, c396 INT8, c397 INT8, c398 INT8, c399 INT8, c400 INT8, c401 INT8, c402 INT8, c403 INT8, c404 INT8, c405 INT8, c406 INT8, c407 INT8, c408 INT8, c409 INT8, c410 INT8, c411 INT8, c412 INT8, c413 INT8, c414 INT8, c415 INT8, c416 INT8, c417 INT8, c418 INT8, c419 INT8, c420 INT8, c421 INT8, c422 INT8, c423 INT8, c424 INT8, c425 INT8, c426 INT8, c427 INT8, c428 INT8, c429 INT8, c430 INT8, c431 INT8, c432 INT8, c433 INT8, c434 INT8, c435 INT8, c436 INT8, c437 INT8, c438 INT8, c439 INT8, c440 INT8, c441 INT8, c442 INT8, c443 INT8, c444 INT8, c445 INT8, c446 INT8, c447 INT8, c448 INT8, c449 INT8, c450 INT8, c451 INT8, c452 INT8, c453 INT8, c454 INT8, c455 INT8, c456 INT8, c457 INT8, c458 INT8, c459 INT8, c460 INT8, c461 INT8, c462 INT8, c463 INT8, c464 INT8, c465 INT8, c466 INT8, c467 INT8, c468 INT8, c469 INT8, c470 INT8, c471 INT8, c472 INT8, c473 INT8, c474 INT8, c475 INT8, c476 INT8, c477 INT8, c478 INT8, c479 INT8, c480 INT8, c481 INT8, c482 INT8, c483 INT8, c484 INT8, c485 INT8, c486 INT8, c487 INT8, c488 INT8, c489 INT8, c490 INT8, c491 INT8, c492 INT8, c493 INT8, c494 INT8, c495 INT8, c496 INT8, c497 INT8, c498 INT8, c499 INT8, c500 INT8, c501 INT8, c502 INT8, c503 INT8, c504 INT8, c505 INT8, c506 INT8, c507 INT8, c508 INT8, c509 INT8, c510 INT8, c511 INT8, c512 INT8, c513 INT8, c514 INT8, c515 INT8, c516 INT8, c517 INT8, c518 INT8, c519 INT8, c520 INT8, c521 INT8, c522 INT8, c523 INT8, c524 INT8, c525 INT8, c526 INT8, c527 INT8, c528 INT8, c529 INT8, c530 INT8, c531 INT8, c532 INT8, c533 INT8, c534 INT8, c535 INT8, c536 INT8, c537 INT8, c538 INT8, c539 INT8, c540 INT8, c541 INT8, c542 INT8, c543 INT8, c544 INT8, c545 INT8, c546 INT8, c547 INT8, c548 INT8, c549 INT8, c550 INT8, c551 INT8, c552 INT8, c553 INT8, c554 INT8, c555 INT8, c556 INT8, c557 INT8, c558 INT8, c559 INT8, c560 INT8, c561 INT8, c562 INT8, c563 INT8, c564 INT8, c565 INT8, c566 INT8, c567 INT8, c568 INT8, c569 INT8, c570 INT8, c571 INT8, c572 INT8, c573 INT8, c574 INT8, c575 INT8, c576 INT8, c577 INT8, c578 INT8, c579 INT8, c580 INT8, c581 INT8, c582 INT8, c583 INT8, c584 INT8, c585 INT8, c586 INT8, c587 INT8, c588 INT8, c589 INT8, c590 INT8, c591 INT8, c592 INT8, c593 INT8, c594 INT8, c595 INT8, c596 INT8, c597 INT8, c598 INT8, c599 INT8, c600 INT8, c601 INT8, c602 INT8, c603 INT8, c604 INT8, c605 INT8, c606 INT8, c607 INT8, c608 INT8, c609 INT8, c610 INT8, c611 INT8, c612 INT8, c613 INT8, c614 INT8, c615 INT8, c616 INT8, c617 INT8, c618 INT8, c619 INT8, c620 INT8, c621 INT8, c622 INT8, c623 INT8, c624 INT8, c625 INT8, c626 INT8, c627 INT8, c628 INT8, c629 INT8, c630 INT8, c631 INT8, c632 INT8, c633 INT8, c634 INT8, c635 INT8, c636 INT8, c637 INT8, c638 INT8, c639 INT8, c640 INT8, c641 INT8, c642 INT8, c643 INT8, c644 INT8, c645 INT8, c646 INT8, c647 INT8, c648 INT8, c649 INT8, c650 INT8, c651 INT8, c652 INT8, c653 INT8, c654 INT8, c655 INT8, c656 INT8, c657 INT8, c658 INT8, c659 INT8, c660 INT8, c661 INT8, c662 INT8, c663 INT8, c664 INT8, c665 INT8, c666 INT8, c667 INT8, c668 INT8, c669 INT8, c670 INT8, c671 INT8, c672 INT8, c673 INT8, c674 INT8, c675 INT8, c676 INT8, c677 INT8, c678 INT8, c679 INT8, c680 INT8, c681 INT8, c682 INT8, c683 INT8, c684 INT8, c685 INT8, c686 INT8, c687 INT8, c688 INT8, c689 INT8, c690 INT8, c691 INT8, c692 INT8, c693 INT8, c694 INT8, c695 INT8, c696 INT8, c697 INT8, c698 INT8, c699 INT8, c700 INT8, c701 INT8, c702 INT8, c703 INT8, c704 INT8, c705 INT8, c706 INT8, c707 INT8, c708 INT8, c709 INT8, c710 INT8, c711 INT8, c712 INT8, c713 INT8, c714 INT8, c715 INT8, c716 INT8, c717 INT8, c718 INT8, c719 INT8, c720 INT8, c721 INT8, c722 INT8, c723 INT8, c724 INT8, c725 INT8, c726 INT8, c727 INT8, c728 INT8, c729 INT8, c730 INT8, c731 INT8, c732 INT8, c733 INT8, c734 INT8, c735 INT8, c736 INT8, c737 INT8, c738 INT8, c739 INT8, c740 INT8, c741 INT8, c742 INT8, c743 INT8, c744 INT8, c745 INT8, c746 INT8, c747 INT8, c748 INT8, c749 INT8, c750 INT8, c751 INT8, c752 INT8, c753 INT8, c754 INT8, c755 INT8, c756 INT8, c757 INT8, c758 INT8, c759 INT8, c760 INT8, c761 INT8, c762 INT8, c763 INT8, c764 INT8, c765 INT8, c766 INT8, c767 INT8, c768 INT8, c769 INT8, c770 INT8, c771 INT8, c772 INT8, c773 INT8, c774 INT8, c775 INT8, c776 INT8, c777 INT8, c778 INT8, c779 INT8, c780 INT8, c781 INT8, c782 INT8, c783 INT8, c784 INT8, c785 INT8, c786 INT8, c787 INT8, c788 INT8, c789 INT8, c790 INT8, c791 INT8, c792 INT8, c793 INT8, c794 INT8, c795 INT8, c796 INT8, c797 INT8, c798 INT8, c799 INT8, c800 INT8, c801 INT8, c802 INT8, c803 INT8, c804 INT8, c805 INT8, c806 INT8, c807 INT8, c808 INT8, c809 INT8, c810 INT8, c811 INT8, c812 INT8, c813 INT8, c814 INT8, c815 INT8, c816 INT8, c817 INT8, c818 INT8, c819 INT8, c820 INT8, c821 INT8, c822 INT8, c823 INT8, c824 INT8, c825 INT8, c826 INT8, c827 INT8, c828 INT8, c829 INT8, c830 INT8, c831 INT8, c832 INT8, c833 INT8, c834 INT8, c835 INT8, c836 INT8, c837 INT8, c838 INT8, c839 INT8, c840 INT8, c841 INT8, c842 INT8, c843 INT8, c844 INT8, c845 INT8, c846 INT8, c847 INT8, c848 INT8, c849 INT8, c850 INT8, c851 INT8, c852 INT8, c853 INT8, c854 INT8, c855 INT8, c856 INT8, c857 INT8, c858 INT8, c859 INT8, c860 INT8, c861 INT8, c862 INT8, c863 INT8, c864 INT8, c865 INT8, c866 INT8, c867 INT8, c868 INT8, c869 INT8, c870 INT8, c871 INT8, c872 INT8, c873 INT8, c874 INT8, c875 INT8, c876 INT8, c877 INT8, c878 INT8, c879 INT8, c880 INT8, c881 INT8, c882 INT8, c883 INT8, c884 INT8, c885 INT8, c886 INT8, c887 INT8, c888 INT8, c889 INT8, c890 INT8, c891 INT8, c892 INT8, c893 INT8, c894 INT8, c895 INT8, c896 INT8, c897 INT8, c898 INT8, c899 INT8, c900 INT8, c901 INT8, c902 INT8, c903 INT8, c904 INT8, c905 INT8, c906 INT8, c907 INT8, c908 INT8, c909 INT8, c910 INT8, c911 INT8, c912 INT8, c913 INT8, c914 INT8, c915 INT8, c916 INT8, c917 INT8, c918 INT8, c919 INT8, c920 INT8, c921 INT8, c922 INT8, c923 INT8, c924 INT8, c925 INT8, c926 INT8, c927 INT8, c928 INT8, c929 INT8, c930 INT8, c931 INT8, c932 INT8, c933 INT8, c934 INT8, c935 INT8, c936 INT8, c937 INT8, c938 INT8, c939 INT8, c940 INT8, c941 INT8, c942 INT8, c943 INT8, c944 INT8, c945 INT8, c946 INT8, c947 INT8, c948 INT8, c949 INT8, c950 INT8, c951 INT8, c952 INT8, c953 INT8, c954 INT8, c955 INT8, c956 INT8, c957 INT8, c958 INT8, c959 INT8, c960 INT8, c961 INT8, c962 INT8, c963 INT8, c964 INT8, c965 INT8, c966 INT8, c967 INT8, c968 INT8, c969 INT8, c970 INT8, c971 INT8, c972 INT8, c973 INT8, c974 INT8, c975 INT8, c976 INT8, c977 INT8, c978 INT8, c979 INT8, c980 INT8, c981 INT8, c982 INT8, c983 INT8, c984 INT8, c985 INT8, c986 INT8, c987 INT8, c988 INT8, c989 INT8, c990 INT8, c991 INT8, c992 INT8, c993 INT8, c994 INT8, c995 INT8, c996 INT8, c997 INT8, c998 INT8, c999 INT8, c1000 INT8, c1001 INT8, c1002 INT8, c1003 INT8, c1004 INT8, c1005 INT8, c1006 INT8, c1007 INT8, c1008 INT8, c1009 INT8, c1010 INT8, c1011 INT8, c1012 INT8, c1013 INT8, c1014 INT8, c1015 INT8, c1016 INT8, c1017 INT8, c1018 INT8, c1019 INT8);",
                "CREATE TABLE k (a TEXT STORAGE PLAIN, b TEXT STORAGE PLAIN);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (1, repeat('x', 20000));",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "row is too big: size 20032, maximum size 8160", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (2, repeat('x', 8128));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (3, repeat('x', 8129));",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "row is too big: size 8168, maximum size 8160", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (4, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET t = repeat('y', 9000) WHERE id = 4;",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "row is too big: size 9032, maximum size 8160", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, length(t) FROM p ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("2"), T("8128")],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO e VALUES (1, repeat('x', 20000), repeat('m', 20000), repeat('e', 20000));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, length(t), length(m), length(x) FROM e;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("length", INT4), Column("length", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("1"), T("20000"), T("20000"), T("20000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO w (c0) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO w SELECT 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1;",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "row is too big: size 8184, maximum size 8160", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO w SELECT 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1;",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "row is too big: size 8272, maximum size 8160", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO k VALUES (repeat('a', 4100), repeat('b', 4100));",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "row is too big: size 8232, maximum size 8160", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO k VALUES (repeat('a', 4000), repeat('b', 4100));",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_on_conflict_with_exclusion_constraint_arbiters() {
    run_scripts(&[
        ScriptTest {
            name: "ON CONFLICT DO NOTHING with an exclusion constraint as its arbiter on a table without a primary key",
            set_up_script: &[
                "CREATE TABLE circles (c1 CIRCLE, c2 TEXT, EXCLUDE USING gist (c1 WITH &&, (c2::circle) WITH &&) WHERE (circle_center(c1) <> '(0,0)'))",
                "INSERT INTO circles VALUES ('<(0,0), 5>', '<(0,0), 5>')",
                "INSERT INTO circles VALUES ('<(20,20), 10>', '<(0,0), 4>')",
                "CREATE TABLE temporal3 (id int4range, valid_at daterange, CONSTRAINT temporal3_uq UNIQUE (id, valid_at WITHOUT OVERLAPS))",
                "INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2000-01-01', '2010-01-01'))",
                "CREATE TABLE ex (a int, EXCLUDE USING btree (a WITH =))",
                "INSERT INTO ex VALUES (1)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO circles VALUES ('<(20,20), 10>', '<(0,0), 4>') ON CONFLICT ON CONSTRAINT circles_c1_c2_excl DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO circles VALUES ('<(20,20), 10>', '<(0,0), 4>') ON CONFLICT ON CONSTRAINT circles_c1_c2_excl DO UPDATE SET c2 = EXCLUDED.c2;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "ON CONFLICT DO UPDATE not supported with exclusion constraints", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO temporal3 (id, valid_at) VALUES ('[1,2)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT ON CONSTRAINT temporal3_uq DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO temporal3 (id, valid_at) VALUES ('[2,3)', daterange('2005-01-01', '2006-01-01')) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM temporal3 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4RANGE), Column("valid_at", DATERANGE)],
                        rows: &[
                            &[T("[1,2)"), T("[2000-01-01,2010-01-01)")],
                            &[T("[2,3)"), T("[2005-01-01,2006-01-01)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex VALUES (1) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex VALUES (2) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM ex ORDER BY a;",
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
fn test_identical_rows_of_tables_without_a_primary_key_wait_for_each_other_under_unique_and_exclusion_constraints() {
    run_scripts(&[
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a unique key waits and fails once the other commits",
            set_up_script: &[
                "CREATE TABLE ku (v INTEGER UNIQUE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ku VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ku VALUES (5);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "ku_v_key""#, ..E }),
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
                    query: "SELECT count(*) FROM ku;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a unique key waits and succeeds once the other rolls back",
            set_up_script: &[
                "CREATE TABLE ku (v INTEGER UNIQUE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ku VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ku VALUES (5);",
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
                    query: "SELECT count(*) FROM ku;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a deferred unique key waits and fails once the other commits",
            set_up_script: &[
                "CREATE TABLE kd (v INTEGER UNIQUE DEFERRABLE INITIALLY DEFERRED);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kd VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kd VALUES (5);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "kd_v_key""#, ..E }),
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
                    query: "SELECT count(*) FROM kd;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a deferred unique key waits and succeeds once the other rolls back",
            set_up_script: &[
                "CREATE TABLE kd (v INTEGER UNIQUE DEFERRABLE INITIALLY DEFERRED);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kd VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kd VALUES (5);",
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
                    query: "SELECT count(*) FROM kd;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a btree exclusion constraint waits and fails once the other commits",
            set_up_script: &[
                "CREATE TABLE kx (r INTEGER, EXCLUDE USING btree (r WITH =));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kx VALUES (7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kx VALUES (7);",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: r#"conflicting key value violates exclusion constraint "kx_r_excl""#, ..E }),
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
                    query: "SELECT count(*) FROM kx;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a btree exclusion constraint waits and succeeds once the other rolls back",
            set_up_script: &[
                "CREATE TABLE kx (r INTEGER, EXCLUDE USING btree (r WITH =));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kx VALUES (7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kx VALUES (7);",
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
                    query: "SELECT count(*) FROM kx;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a gist exclusion constraint waits and fails once the other commits",
            set_up_script: &[
                "CREATE TABLE kg (r int4range, EXCLUDE USING gist (r WITH &&));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kg VALUES ('[1,10)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kg VALUES ('[1,10)');",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: r#"conflicting key value violates exclusion constraint "kg_r_excl""#, ..E }),
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
                    query: "SELECT count(*) FROM kg;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a gist exclusion constraint waits and succeeds once the other rolls back",
            set_up_script: &[
                "CREATE TABLE kg (r int4range, EXCLUDE USING gist (r WITH &&));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kg VALUES ('[1,10)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kg VALUES ('[1,10)');",
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
                    query: "SELECT count(*) FROM kg;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a unique key a multi-row insert meets waits and fails once the other commits",
            set_up_script: &[
                "CREATE TABLE km (v INTEGER UNIQUE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO km VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO km VALUES (4), (5), (6);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "km_v_key""#, ..E }),
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
                    query: "SELECT count(*) FROM km;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a unique key a multi-row insert meets waits and succeeds once the other rolls back",
            set_up_script: &[
                "CREATE TABLE km (v INTEGER UNIQUE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO km VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO km VALUES (4), (5), (6);",
                    expected: Expected::Tag("INSERT 0 3"),
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
                    query: "SELECT count(*) FROM km;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("3")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a unique key a COPY meets waits and fails once the other commits",
            set_up_script: &[
                "CREATE TABLE kc (v INTEGER UNIQUE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kc VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY kc FROM PROGRAM 'echo 5';",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "kc_v_key""#, ..E }),
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
                    query: "SELECT count(*) FROM kc;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a unique key a COPY meets waits and succeeds once the other rolls back",
            set_up_script: &[
                "CREATE TABLE kc (v INTEGER UNIQUE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kc VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY kc FROM PROGRAM 'echo 5';",
                    expected: Expected::Tag("COPY 1"),
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
                    query: "SELECT count(*) FROM kc;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a partition's unique key waits and fails once the other commits",
            set_up_script: &[
                "CREATE TABLE kp (v INTEGER, UNIQUE (v)) PARTITION BY RANGE (v);",
                "CREATE TABLE kp1 PARTITION OF kp FOR VALUES FROM (0) TO (100);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kp VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kp VALUES (5);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "kp1_v_key""#, ..E }),
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
                    query: "SELECT count(*) FROM kp;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a partition's unique key waits and succeeds once the other rolls back",
            set_up_script: &[
                "CREATE TABLE kp (v INTEGER, UNIQUE (v)) PARTITION BY RANGE (v);",
                "CREATE TABLE kp1 PARTITION OF kp FOR VALUES FROM (0) TO (100);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kp VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kp VALUES (5);",
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
                    query: "SELECT count(*) FROM kp;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a unique key an update meets waits and fails once the other commits",
            set_up_script: &[
                "CREATE TABLE kup (v INTEGER UNIQUE);",
                "INSERT INTO kup VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kup VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE kup SET v = 5 WHERE v = 1;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "kup_v_key""#, ..E }),
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
                    query: "SELECT count(*) FROM kup;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("2")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a unique key an update meets waits and succeeds once the other rolls back",
            set_up_script: &[
                "CREATE TABLE kup (v INTEGER UNIQUE);",
                "INSERT INTO kup VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO kup VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE kup SET v = 5 WHERE v = 1;",
                    expected: Expected::Tag("UPDATE 1"),
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
                    query: "SELECT count(*) FROM kup;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a unique key an ON CONFLICT DO NOTHING insert meets waits and does nothing once the other commits",
            set_up_script: &[
                "CREATE TABLE ko (v INTEGER UNIQUE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ko VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ko VALUES (5) ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
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
                    query: "SELECT count(*) FROM ko;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a row of a table without a primary key identical to another transaction's pending one under a unique key an ON CONFLICT DO NOTHING insert meets waits and succeeds once the other rolls back",
            set_up_script: &[
                "CREATE TABLE ko (v INTEGER UNIQUE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ko VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ko VALUES (5) ON CONFLICT DO NOTHING;",
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
                    query: "SELECT count(*) FROM ko;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}


#[test]
fn test_identical_rows_of_tables_without_a_primary_key_are_checked_at_commit_under_deferred_exclusion_constraints() {
    run_scripts(&[
        ScriptTest {
            name: "an identical row of a table without a primary key under a deferred btree exclusion constraint that another transaction is adding fails at its commit once the other commits",
            set_up_script: &[
                "CREATE TABLE dx (r INTEGER, EXCLUDE USING btree (r WITH =) DEFERRABLE INITIALLY DEFERRED);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dx VALUES (7);",
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
                    query: "INSERT INTO dx VALUES (7);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: r#"conflicting key value violates exclusion constraint "dx_r_excl""#, detail: "Key (r)=(7) conflicts with existing key (r)=(7).", schema: "public", table: "dx", constraint: "dx_r_excl", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dx;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an identical row of a table without a primary key under a deferred btree exclusion constraint that another transaction is adding commits once the other rolls back",
            set_up_script: &[
                "CREATE TABLE dx (r INTEGER, EXCLUDE USING btree (r WITH =) DEFERRABLE INITIALLY DEFERRED);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dx VALUES (7);",
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
                    query: "INSERT INTO dx VALUES (7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
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
                    query: "SELECT count(*) FROM dx;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an identical row of a table without a primary key under a deferred gist exclusion constraint that another transaction is adding fails at its commit once the other commits",
            set_up_script: &[
                "CREATE TABLE dg (r int4range, EXCLUDE USING gist (r WITH &&) DEFERRABLE INITIALLY DEFERRED);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dg VALUES ('[1,10)');",
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
                    query: "INSERT INTO dg VALUES ('[1,10)');",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "23P01", message: r#"conflicting key value violates exclusion constraint "dg_r_excl""#, detail: "Key (r)=([1,10)) conflicts with existing key (r)=([1,10)).", schema: "public", table: "dg", constraint: "dg_r_excl", ..E }),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dg;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an identical row of a table without a primary key under a deferred gist exclusion constraint that another transaction is adding commits once the other rolls back",
            set_up_script: &[
                "CREATE TABLE dg (r int4range, EXCLUDE USING gist (r WITH &&) DEFERRABLE INITIALLY DEFERRED);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dg VALUES ('[1,10)');",
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
                    query: "INSERT INTO dg VALUES ('[1,10)');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
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
                    query: "SELECT count(*) FROM dg;",
                    expected: Expected::Rows { columns: &[Column("count", INT8)], rows: &[&[T("1")]], tag: "SELECT 1" },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
