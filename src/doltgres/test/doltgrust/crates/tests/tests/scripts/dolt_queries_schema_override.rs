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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_schema_override.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts};

#[test]
fn test_schema_override_tests() {
    run_scripts(&[
        ScriptTest {
            name: "Basic Case: Dropping a column and adding a column",
            intent: "With dolt_override_schema set to a commit, reads use that commit's columns over the current rows, dropping and filling columns as needed.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-am', 'dropping column c1 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t (pk, c2) VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'adding column c2 on main');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit3', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 't' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("integer"), T("NO")],
                            &[T("c2"), T("character varying"), T("YES")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE is MySQL's; information_schema.columns lists the overridden table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM t;",
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
                    query: "SELECT pk, 42, c1 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("?column?", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("42"), Null],
                            &[T("2"), T("42"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET dolt_override_schema;",
                    expected: Expected::Tag("RESET"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's SET ... = NULL is RESET")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", VARCHAR)],
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
        ScriptTest {
            name: "Basic Case: Adding columns",
            intent: "An overridden schema with more columns reads the current rows with NULL in the added columns.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t VALUES (2, 'two', 'zwei');",
                "SELECT dolt_commit('-am', 'adding column c2 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c3 varchar(255);",
                "INSERT INTO t VALUES (3, 'three', 'drei', 'tres');",
                "SELECT dolt_commit('-am', 'adding column c3 on main');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit3', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR), Column("c3", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one"), Null, Null],
                            &[T("2"), T("two"), T("zwei"), Null],
                            &[T("3"), T("three"), T("drei"), T("tres")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit2', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one"), Null],
                            &[T("2"), T("two"), T("zwei")],
                            &[T("3"), T("three"), T("drei")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                            &[T("3"), T("three")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Basic Case: Dropping columns",
            intent: "An overridden schema with fewer columns reads the current rows without the dropped columns.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(10), c2 int NOT NULL, c3 float, c4 text, c5 int, c6 varchar(10), c7 int);",
                "INSERT INTO t VALUES (1, 'one', 2, 3.0, 'four', 5, 'six', 7);",
                "SELECT dolt_commit('-Am', 'adding table t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c5;",
                "SELECT dolt_commit('-am', 'dropping column c5');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c2;",
                "SELECT dolt_commit('-am', 'dropping column c2');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit2', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", INT4), Column("c3", FLOAT8), Column("c4", TEXT), Column("c6", VARCHAR), Column("c7", INT4)],
                        rows: &[
                            &[T("1"), T("one"), Null, T("3"), T("four"), T("six"), T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("c2", INT4), Column("c3", FLOAT8), Column("c4", TEXT), Column("c5", INT4), Column("c6", VARCHAR), Column("c7", INT4)],
                        rows: &[
                            &[T("1"), T("one"), Null, T("3"), T("four"), Null, T("six"), T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Basic Case: Renaming columns",
            intent: "An overridden schema with renamed columns reads the current rows under the old names.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t RENAME COLUMN c1 TO c2;",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'renaming column c1 to c2 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit2', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
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
            name: "Error Case: Invalid schema override commit",
            intent: "Reads fail when dolt_override_schema names a missing branch or a malformed revision.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-am', 'dropping column c1 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t (pk, c2) VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'adding column c2 on main');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_override_schema = doesNotExist;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to resolve schema override value: branch not found: doesnotexist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_override_schema = POINT(100, 42);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"(\"", position: 33, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_override_schema = 'HEA~D~^~~~';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid commit spec specified in dolt_override_schema: Invalid HEAD spec: ~D~^~~~", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "PK Change: Adding a column to PK",
            intent: "An overridden schema whose primary key has another column reads the current rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN pk2 int;",
                "UPDATE t SET pk2 = 1;",
                "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                "INSERT INTO t VALUES (2, 'two', 2);",
                "SELECT dolt_commit('-am', 'adding a column to the PK');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
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
            name: "PK Change: Dropping a column from PK",
            intent: "An overridden schema whose primary key lacks a column reads the current rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int, c1 varchar(255), pk2 int, PRIMARY KEY(pk, pk2));",
                "INSERT INTO t VALUES (1, 'one', 1);",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                "ALTER TABLE t DROP COLUMN pk2;",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'dropping a column from the PK');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("pk2", INT4)],
                        rows: &[
                            &[T("1"), T("one"), Null],
                            &[T("2"), T("two"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Type Change: Compatible data",
            intent: "An overridden column type converts the current values when they fit.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ALTER COLUMN c1 TYPE TEXT, ALTER COLUMN c1 DROP NOT NULL, ALTER COLUMN c1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'adding column c2 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit2', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
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
            name: "Type Change: Truncated data",
            intent: "An overridden shorter varchar truncates the current values with a warning.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(5));",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding tables t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ALTER COLUMN c1 TYPE varchar(100), ALTER COLUMN c1 DROP NOT NULL, ALTER COLUMN c1 DROP DEFAULT;",
                "INSERT INTO t VALUES (2, 'twotwotwotwotwotwo');",
                "SELECT dolt_commit('-am', 'modifying columns in t on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit2', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("twotwotwotwotwotwo")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("twotw")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Type Change: Incompatible data",
            intent: "Reads fail when the current values can't convert to the overridden column type.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 point);",
                "INSERT INTO t VALUES (1, POINT(1, 1));",
                "SELECT dolt_commit('-Am', 'adding tables t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "ALTER TABLE t ADD COLUMN c1 int;",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'modifying columns in t on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit2', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Error(Diagnostic { code: "42846", message: "unable to convert value to overridden schema: cannot cast type integer to point", detail: "Column \"c1\" of relation \"t\".", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Table Existence: Table exists in the data commit, NOT in pinned schema commit",
            intent: "A table missing from the overridden schema's commit can't be read.",
            set_up_script: &[
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "CREATE TABLE addedTable (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO addedTable VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table addedTable on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM addedTable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM addedTable;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "unable to find table \"addedtable\" at overridden schema root", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Table Existence: Table exists in the pinned schema commit, NOT in data commit",
            intent: "A table only the overridden schema's commit has reads as empty.",
            set_up_script: &[
                "CREATE TABLE deletedTable (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO deletedTable VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table deletedTable on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "DROP TABLE deletedTable;",
                "SELECT dolt_commit('-Am', 'deleting table deletedTable on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM deletedTable;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"deletedtable\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM deletedTable;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Table Existence: Table exists in the pinned schema commit, NOT in AS OF data commit",
            intent: "A table only the overridden schema's commit has reads as empty AS OF a commit without it.",
            set_up_script: &[
                "CREATE TABLE deletedTable (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO deletedTable VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table deletedTable on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "DROP TABLE deletedTable;",
                "SELECT dolt_commit('-Am', 'deleting table deletedTable on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM deletedTable;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"deletedtable\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM deletedTable AS OF current_setting('test.commit2', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Indexes: Index in the pinned schema, but not in the data commit",
            intent: "An index only the overridden schema has still gives the right rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255)); CREATE INDEX c1_idx ON t (c1);",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t with index on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "DROP INDEX c1_idx;",
                "UPDATE t SET c1='two';",
                "SELECT dolt_commit('-Am', 'adding table t with index on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT c1 FROM t AS OF current_setting('test.commit1', TRUE) WHERE c1 > 'o';",
                    expected: Expected::Rows {
                        columns: &[Column("c1", VARCHAR)],
                        rows: &[
                            &[T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c1 FROM t WHERE c1 > 'o';",
                    expected: Expected::Rows {
                        columns: &[Column("c1", VARCHAR)],
                        rows: &[
                            &[T("two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c1 FROM t WHERE c1 > 'o';",
                    expected: Expected::Rows {
                        columns: &[Column("c1", VARCHAR)],
                        rows: &[
                            &[T("two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit2', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c1 FROM t AS OF current_setting('test.commit1', TRUE) WHERE c1 > 'o';",
                    expected: Expected::Rows {
                        columns: &[Column("c1", VARCHAR)],
                        rows: &[
                            &[T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "AS OF: schema pinning with AS OF",
            intent: "An overridden schema applies to reads AS OF older commits.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-am', 'dropping column c1 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t (pk, c2) VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'adding column c2 on main');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit3', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF current_setting('test.commit1', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF current_setting('test.commit2', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF current_setting('test.commit3', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Revision DBs: branch revision database with overridden schema",
            intent: "An overridden schema applies to reads through a branch's revision database.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT dolt_branch('branch1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-am', 'dropping column c1 on main');",
                "SELECT dolt_branch('branch2');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t (pk, c2) VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'adding column c2 on main');",
                "SELECT dolt_branch('branch3');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit3', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/branch3\".public.t AS OF current_setting('test.commit1', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/branch3\".public.t AS OF current_setting('test.commit2', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/branch2\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Revision DBs: read-only tag revision database with overridden schema",
            intent: "An overridden schema applies to reads through a tag's revision database.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_tag('commit1');",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-am', 'dropping column c1 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_tag('commit2');",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t (pk, c2) VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'adding column c2 on main');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_tag('commit3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit3', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/commit3\".public.t AS OF current_setting('test.commit1', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/commit3\".public.t AS OF current_setting('test.commit2', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/commit2\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Revision DBs: ancestor syntax revision database with overridden schema",
            intent: "An overridden schema applies to reads through an ancestor revision database.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-am', 'dropping column c1 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t (pk, c2) VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'adding column c2 on main');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit3', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF current_setting('test.commit1', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF current_setting('test.commit2', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main~\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Joins: Two tables with changed schemas and renamed primary key",
            intent: "A join of two tables reads both under the overridden schema, renamed key included.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c1 varchar(255));",
                "CREATE TABLE t2 (pk int PRIMARY KEY, c1 int, c2 varchar(100));",
                "INSERT INTO t1 VALUES (1, 'one');",
                "INSERT INTO t2 VALUES (100, 1, 'blue');",
                "SELECT dolt_commit('-Am', 'adding tables t1 and t2 on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t1 RENAME COLUMN pk TO newPk;",
                "ALTER TABLE t1 RENAME COLUMN c1 TO c2;",
                "ALTER TABLE t2 ALTER COLUMN c1 TYPE varchar(100), ALTER COLUMN c1 DROP NOT NULL, ALTER COLUMN c1 DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'modifying columns in t1 and t2 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 JOIN t2 ON t1.pk = t2.c1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR), Column("pk", INT4), Column("c1", INT4), Column("c2", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one"), T("100"), T("1"), T("blue")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Read-only: Write queries are not allowed when the schema is overridden",
            intent: "Writes and DDL are refused while the schema is overridden, and allowed again after the override is reset.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, c1 varchar(5));",
                "INSERT INTO t1 VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding tables t1 and t2 on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t1 ALTER COLUMN c1 TYPE varchar(100), ALTER COLUMN c1 DROP NOT NULL, ALTER COLUMN c1 DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'modifying columns in t1 and t2 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (pk int PRIMARY KEY, c1 JSON);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t3 (pk int PRIMARY KEY, c1 JSON);",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute CREATE TABLE while dolt_override_schema is set", ..E }),
                    dolt: Some(Dolt::Differs("Postgres' read-only wording, with the setting that makes the session read-only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (3, NULL);",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT while dolt_override_schema is set", ..E }),
                    dolt: Some(Dolt::Differs("Postgres' read-only wording, with the setting that makes the session read-only")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET dolt_override_schema;",
                    expected: Expected::Tag("RESET"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's SET ... = NULL is RESET")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (3, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "System Tables: schema overrides do not apply to system tables",
            intent: "System tables keep their own columns while the schema is overridden.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-am', 'dropping column c1 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t (pk, c2) VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'adding column c2 on main');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, dolt_commit, dolt_committer, dolt_message FROM dolt_blame_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("dolt_commit", VARCHAR), Column("dolt_committer", TEXT), Column("dolt_message", TEXT)],
                        rows: &[
                            &[T("1"), Any, T("postgres"), T("dropping column c1 on main")],
                            &[T("2"), Any, T("postgres"), T("adding column c2 on main")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root; ours as the session's user, postgres")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c2, to_pk, to_c2, dolt_from_commit, dolt_to_commit, dolt_diff_type FROM dolt_diff_t;",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c2", VARCHAR), Column("to_pk", INT4), Column("to_c2", VARCHAR), Column("dolt_from_commit", TEXT), Column("dolt_to_commit", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[Null, Null, T("2"), T("two"), Any, Any, T("added")],
                            &[T("1"), Null, T("1"), Null, Any, Any, T("modified")],
                            &[Null, Null, T("1"), Null, Any, Any, T("added")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_c2, dolt_to_commit, from_pk, from_c2, dolt_from_commit, dolt_diff_type FROM dolt_commit_diff_t WHERE dolt_from_commit=current_setting('test.commit1', TRUE) AND dolt_to_commit=current_setting('test.commit3', TRUE);",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_c2", VARCHAR), Column("dolt_to_commit", TEXT), Column("from_pk", INT4), Column("from_c2", VARCHAR), Column("dolt_from_commit", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), Null, Any, T("1"), Null, Any, T("modified")],
                            &[T("2"), T("two"), Any, Null, Null, Any, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c2, dolt_commit_hash, dolt_committer FROM dolt_history_t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c2", VARCHAR), Column("dolt_commit_hash", TEXT), Column("dolt_committer", TEXT)],
                        rows: &[
                            &[T("1"), Null, Any, T("postgres")],
                            &[T("2"), T("two"), Any, T("postgres")],
                            &[T("1"), Null, Any, T("postgres")],
                            &[T("1"), Null, Any, T("postgres")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root; ours as the session's user, postgres")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff(): schema overrides should apply",
            intent: "dolt_diff shows rows under the overridden schema's columns.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-am', 'dropping column c1 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t (pk, c2) VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'adding column c2 on main');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, to_pk, to_c1, dolt_diff_type FROM dolt_diff(current_setting('test.commit1', TRUE), current_setting('test.commit3', TRUE), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("from_c1", VARCHAR), Column("to_pk", INT4), Column("to_c1", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("one"), T("1"), Null, T("modified")],
                            &[Null, Null, T("2"), Null, T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff(): override commit missing table",
            intent: "dolt_diff of a table the overridden schema's commit lacks fails.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t (pk, c1, c2) VALUES (2, 'two', 'val2');",
                "SELECT dolt_commit('-am', 'adding column c2');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "DROP TABLE t;",
                "CREATE TABLE other_t (pk int PRIMARY KEY);",
                "INSERT INTO other_t (pk) VALUES (1);",
                "SELECT dolt_commit('-am', 'dropping t, adding other_t');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit3', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, from_c1, to_pk, to_c2, dolt_diff_type FROM dolt_diff(current_setting('test.commit1', TRUE), current_setting('test.commit2', TRUE), 't');",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "unable to find table \"t\" at overridden schema root", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_diff(): override commit between from and to",
            intent: "dolt_diff uses the overridden schema even when its commit lies between the diff's two commits.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'commit1: pk, c1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-am', 'commit2: drop c1');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t (pk, c2) VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'commit3: add c2');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit2', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_pk, to_pk, dolt_diff_type FROM dolt_diff(current_setting('test.commit1', TRUE), current_setting('test.commit3', TRUE), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_pk", INT4), Column("to_pk", INT4), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("modified")],
                            &[Null, T("2"), T("added")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_c1, dolt_diff_type FROM dolt_diff(current_setting('test.commit1', TRUE), current_setting('test.commit2', TRUE), 't');",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"from_c1\" does not exist", hint: "Perhaps you meant to reference the column \"dolt_diff.from_pk\".", position: 8, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_patch(): schema overrides should apply",
            intent: "dolt_patch writes statements under the overridden schema.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-am', 'dropping column c1 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t (pk, c2) VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'adding column c2 on main');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT statement FROM dolt_patch(current_setting('test.commit1', TRUE), current_setting('test.commit3', TRUE), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("UPDATE `t` SET `c1`=NULL WHERE `pk`=1;")],
                            &[T("INSERT INTO `t` (`pk`,`c1`) VALUES (2,NULL);")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("Dolt main fails it too: its dolt_patch ignores the override, and Dolt skips its own test"),
            ..S
        },
        ScriptTest {
            name: "dolt_diff_summary(): schema overrides should apply",
            intent: "dolt_diff_summary reports changes under the overridden schema.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t (pk, c1) VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t on main');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "SELECT dolt_commit('-am', 'dropping column c1 on main');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t (pk, c2) VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'adding column c2 on main');",
                "SELECT set_config('test.commit3', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_table_name, to_table_name, diff_type, data_change, schema_change FROM dolt_diff_summary(current_setting('test.commit1', TRUE), current_setting('test.commit3', TRUE), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT), Column("data_change", BOOL), Column("schema_change", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("modified"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("Dolt main fails it too: its dolt_diff_summary ignores the override"),
            ..S
        },
        ScriptTest {
            name: "reading a user table under a dolt_override_schema that names no commit",
            intent: "SET accepts any revision, and a read of a user table under it fails as Dolt's resolution of the setting fails, while the catalogs stay readable.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_override_schema = 'doesNotExist';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unable to resolve schema override value: branch not found: doesNotExist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname = 't';",
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
                    query: "RESET dolt_override_schema;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
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
            name: "Privileges: column grants apply by column id, and a column only the override has needs none",
            intent: "A role with SELECT on some columns reads them under the override by column id, an override-only column needs no privilege, and a column it can't read is refused.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 text, c4 int, c9 int);",
                "INSERT INTO t VALUES (1, 'one', 4, 9);",
                "CREATE ROLE override_reader;",
                "GRANT SELECT (pk, c1) ON t TO override_reader;",
                "SELECT dolt_commit('-Am', 'adding table t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c9;",
                "ALTER TABLE t RENAME COLUMN c1 TO c2;",
                "SELECT dolt_commit('-Am', 'dropping c9 and renaming c1');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE override_reader;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, c1, c9 FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT), Column("c9", INT4)],
                        rows: &[
                            &[T("1"), T("one"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c4 FROM t;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Row security: the table's own policies apply to reads under an override",
            intent: "Reading through an override never bypasses the current table's row-level security.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 text);",
                "INSERT INTO t VALUES (1, 'one'), (2, 'two');",
                "SELECT dolt_commit('-Am', 'adding table t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 int;",
                "ALTER TABLE t ENABLE ROW LEVEL SECURITY;",
                "CREATE POLICY p ON t FOR SELECT USING (pk = 1);",
                "CREATE ROLE rls_reader;",
                "GRANT SELECT ON t TO rls_reader;",
                "SELECT dolt_commit('-Am', 'adding c2 and a policy');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE rls_reader;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "EXPLAIN: a read under an override plans over the table's own indexes",
            intent: "The override's copy is never planned: EXPLAIN shows the scans of the table the rows come from.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 text);",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 int;",
                "SELECT dolt_commit('-Am', 'adding c2');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using t_pkey on t")],
                            &[T("  Index Cond: (pk = 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Views: a view reads its tables under the override by column id",
            intent: "A view's columns follow its table's columns by column id under the override, and a column the override lacks fails the read.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 text);",
                "INSERT INTO t VALUES (1, 'one');",
                "CREATE VIEW v AS SELECT pk, c1 FROM t;",
                "SELECT dolt_commit('-Am', 'adding table t and view v');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t RENAME COLUMN c1 TO c2;",
                "ALTER TABLE t ADD COLUMN c3 int;",
                "CREATE VIEW w AS SELECT pk, c3 FROM t;",
                "INSERT INTO t VALUES (2, 'two', 3);",
                "SELECT dolt_commit('-Am', 'renaming c1, adding c3 and view w');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM v ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM w;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"c3\" of relation \"t\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Type Change: a column dropped and added again under its name converts each value as an explicit cast does",
            intent: "Columns pair by name when their column ids differ, and each value converts as CAST does, failing only for a row that is read.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 int);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'adding table t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "ALTER TABLE t ADD COLUMN c1 text;",
                "INSERT INTO t VALUES (2, '7'), (3, 'x');",
                "SELECT dolt_commit('-Am', 'making c1 text');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t WHERE pk < 3 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4)],
                        rows: &[
                            &[T("1"), Null],
                            &[T("2"), T("7")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"x\"", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Prepared statements: a prepared statement is analyzed again when dolt_override_schema changes",
            intent: "A cached plan records the override it was analyzed under and analyzes again when the setting names another commit or is reset.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 text);",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t ADD COLUMN c2 int;",
                "UPDATE t SET c2 = 2;",
                "SELECT dolt_commit('-Am', 'adding c2');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "PREPARE s AS SELECT c2 FROM t;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE s;",
                    expected: Expected::Rows {
                        columns: &[Column("c2", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE s;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"c2\" does not exist", hint: "Perhaps you meant to reference the column \"t.c1\".", position: 21, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET dolt_override_schema;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE s;",
                    expected: Expected::Rows {
                        columns: &[Column("c2", INT4)],
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
            name: "COPY: COPY TO reads a table under the override's columns",
            intent: "COPY of a table under an override sends the override's columns, as a query of it does, and COPY FROM is refused as every write is.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'replacing c1 with c2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t TO STDOUT (FORMAT csv, HEADER);",
                    expected: Expected::Tag("COPY 2"),
                    copy_to_stdout_file: "schema-override-copy.csv",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY t FROM STDIN (FORMAT csv, HEADER);",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute COPY FROM while dolt_override_schema is set", ..E }),
                    copy_from_stdin_file: "schema-override-copy.csv",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Read-only: a write through a revision-qualified name under an override names the override",
            intent: "A write through a tag's or a branch's qualifier is refused with the override's read-only error while the override is set.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 text);",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_tag('v1');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/v1\".public.t;",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute DELETE in a read-only transaction", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/v1\".public.t;",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute DELETE while dolt_override_schema is set", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2, 'two');",
                    expected: Expected::Error(Diagnostic { code: "25006", message: "cannot execute INSERT while dolt_override_schema is set", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Privileges: a prepared statement under an override takes the privileges of a table dropped and created again",
            intent: "A cached plan under an override is analyzed again when its table is replaced, reading the new table's rows with its privileges.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 text);",
                "INSERT INTO t VALUES (1, 'one');",
                "CREATE ROLE prepared_reader;",
                "GRANT SELECT ON t TO prepared_reader;",
                "SELECT dolt_commit('-Am', 'adding table t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE s AS SELECT * FROM t;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE prepared_reader;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE s;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t;",
                    expected: Expected::Tag("DROP TABLE"),
                    client: "other",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int PRIMARY KEY, c1 text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "other",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (5, 'five');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "other",
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE s;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT SELECT ON t TO prepared_reader;",
                    expected: Expected::Tag("GRANT"),
                    client: "other",
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXECUTE s;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", TEXT)],
                        rows: &[
                            &[T("5"), T("five")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Whole rows: a whole row read under an override has the override's columns, and its system columns are its rows'",
            intent: "A whole-row reference under an override builds the override's row type, nulled by an outer join as a whole, and system columns are those of the rows it reads.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(255));",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'adding table t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "ALTER TABLE t DROP COLUMN c1;",
                "ALTER TABLE t ADD COLUMN c2 varchar(255);",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'replacing c1 with c2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT row_to_json(t) FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("row_to_json", JSON)],
                        rows: &[
                            &[T("{\"pk\":1,\"c1\":null}")],
                            &[T("{\"pk\":2,\"c1\":null}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.pk, b::text FROM t a LEFT JOIN t b ON b.pk = a.pk + 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("(2,)")],
                            &[T("2"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, t FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("t", USER_DEFINED)],
                        rows: &[
                            &[T("(0,1)"), T("(1,)")],
                            &[T("(0,2)"), T("(2,)")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Table Existence: a table of a schema the session's branch lacks has no name under the override",
            intent: "A table the override's commit has in a schema the branch has since dropped isn't named in another schema.",
            set_up_script: &[
                "CREATE SCHEMA s3;",
                "CREATE TABLE s3.t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'add s3.t');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "DROP SCHEMA s3 CASCADE;",
                "SELECT dolt_commit('-Am', 'drop s3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('dolt_override_schema', (current_setting('test.commit1', TRUE))::text, FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s3.t;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"s3.t\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
