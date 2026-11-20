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

//! Dolt's integration-tests/bats/primary-key-changes.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_primary_key_changes() {
    run_scripts(&[
        ScriptTest {
            name: "primary-key-changes: add primary key using null values",
            intent: "A primary key can't be added over a NULL value.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (val) VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "column \"pk\" of relation \"t\" contains null values", schema: "public", table: "t", column: "pk", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: add single primary key",
            intent: "An added primary key refuses a duplicate key.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1),(2,2),(3,3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
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
                    query: "INSERT INTO t VALUES (2,4);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"t_pkey\"", detail: "Key (pk)=(2) already exists.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: add composite primary key",
            intent: "An added two-column primary key refuses a duplicate pair and takes a new one.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk, val);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1),(2,2),(3,3);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
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
                    query: "INSERT INTO t VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"t_pkey\"", detail: "Key (pk, val)=(2, 2) already exists.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("2"), T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM t;",
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
        ScriptTest {
            name: "primary-key-changes: can delete single primary key",
            intent: "Dropping the primary key allows duplicate rows and leaves no key.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int, val int, PRIMARY KEY(pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1),(2,2),(2,2);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE t;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int, val int, PRIMARY KEY(pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 't'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("f")],
                            &[T("val"), T("f"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: can delete composite primary key",
            intent: "Dropping a two-column primary key allows duplicate rows and leaves no key.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int, val int, PRIMARY KEY(pk, val));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1),(2,2),(2,2);",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 't'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("f")],
                            &[T("val"), T("t"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: run through some add and drop primary key operations",
            intent: "A primary key dropped and added on fewer columns refuses duplicates of the new key.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int, val int, PRIMARY KEY(pk, val));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1),(2,2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 5);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"t_pkey\"", detail: "Key (pk)=(1) already exists.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3,3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
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
            name: "primary-key-changes: add an index after dropping a key, and then recreate the key",
            intent: "Primary keys drop and come back around a new secondary index.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int, val int, PRIMARY KEY (pk, val));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk, val);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON t (val);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: alter table on keyless column with duplicates throws an error",
            intent: "A primary key can't be added over duplicate rows.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1),(1,1);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk, val);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"t_pkey\"", detail: "Key (pk, val)=(1, 1) is duplicated.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"t_pkey\"", detail: "Key (pk)=(1) is duplicated.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: dropping a primary key still preserves secondary indexes",
            intent: "Dropping the primary key keeps the table's secondary indexes and rows.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int PRIMARY KEY, val1 int, val2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON t (val2);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1,1), (2,2,2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val1", INT4), Column("val2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3, 3, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val1", INT4), Column("val2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                            &[T("3"), T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 't'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("f")],
                            &[T("val1"), T("f"), T("f")],
                            &[T("val2"), T("f"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: drop on table with no primary key correctly errors",
            intent: "Dropping a primary key a table doesn't have fails.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int, val1 int, val2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "constraint \"t_pkey\" of relation \"t\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: ff merge with primary key schema differences correctly works",
            intent: "A fast-forward merge takes a branch's dropped primary key.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int PRIMARY KEY, val1 int, val2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1,1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test', '-m', 'merge other');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 't'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("f")],
                            &[T("val1"), T("f"), T("f")],
                            &[T("val2"), T("f"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: merge on branch with primary key dropped throws an error",
            intent: "Merging a branch that dropped the primary key fails, and the drop stages as a modified table.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int PRIMARY KEY, val1 int, val2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1,1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'cm2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2,2,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::Differs("dolt merge on the CLI keeps a conflicted merge in the working set, which dolt_allow_commit_conflicts lets dolt_merge do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test', '-m', 'merge other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses the merge with \"cannot merge because table t has different primary keys\", a limitation its merge_schema.go TODO admits, where a merge takes the primary key one side changed to when the other side kept the base's key or made the same change, converting the other side's rows to it, Appendix C \"merges and cherry-picks across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val1", INT4), Column("val2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'p';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses the merge with \"cannot merge because table t has different primary keys\", a limitation its merge_schema.go TODO admits, where a merge takes the primary key one side changed to when the other side kept the base's key or made the same change, converting the other side's rows to it, Appendix C \"merges and cherry-picks across a primary key change\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: merge on branch with primary key added throws an error",
            intent: "Merging a branch that added a primary key fails, and the addition stages as a modified table.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int, val1 int, val2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1,1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'cm2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2,2,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::Differs("dolt merge on the CLI keeps a conflicted merge in the working set, which dolt_allow_commit_conflicts lets dolt_merge do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test', '-m', 'merge other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses the merge with \"cannot merge because table t has different primary keys\", a limitation its merge_schema.go TODO admits, where a merge takes the primary key one side changed to when the other side kept the base's key or made the same change, converting the other side's rows to it, Appendix C \"merges and cherry-picks across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val1", INT4), Column("val2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'p';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("PRIMARY KEY (pk)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses the merge with \"cannot merge because table t has different primary keys\", a limitation its merge_schema.go TODO admits, where a merge takes the primary key one side changed to when the other side kept the base's key or made the same change, converting the other side's rows to it, Appendix C \"merges and cherry-picks across a primary key change\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: merge on branches with different primary key from ancestor throws an error",
            intent: "Merging branches that each added a primary key to a keyless table fails.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t(pk int, val1 int, val2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1,1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'cm2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2,2,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::Differs("dolt merge on the CLI keeps a conflicted merge in the working set, which dolt_allow_commit_conflicts lets dolt_merge do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test', '-m', 'merge other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses the merge with \"cannot merge because table t has different primary keys\", a limitation its merge_schema.go TODO admits, where a merge takes the primary key one side changed to when the other side kept the base's key or made the same change, converting the other side's rows to it, Appendix C \"merges and cherry-picks across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val1", INT4), Column("val2", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'p';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("PRIMARY KEY (pk)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: diff on primary key schema change shows schema level diff but does not show row level diff",
            intent: "Dropping the primary key shows as a schema diff.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    val integer\n);\n\nALTER TABLE public.t OWNER TO postgres;\n\nALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    val integer\n);\n\nALTER TABLE public.t OWNER TO postgres;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: diff on composite schema",
            intent: "Replacing the primary key with a two-column one shows in the schema diff and commits.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk, val);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    val integer\n);\n\nALTER TABLE public.t OWNER TO postgres;\n\nALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk);"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    val integer NOT NULL\n);\n\nALTER TABLE public.t OWNER TO postgres;\n\nALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk, val);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'add changes');",
                    expected: Expected::Ok,
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
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: dolt diff on working set shows correct status diff",
            intent: "A new table whose key was dropped before any commit shows as a new table.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('HEAD', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T(""), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    val integer\n);\n\nALTER TABLE public.t OWNER TO postgres;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: dolt diff table returns top-down diff until schema change",
            intent: "dolt_diff_<table> lists the diffs of the commits after a primary key change and warns about the change.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'cm2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_diff_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("diffs pair rows across a primary key change by a key both versions hold, where Dolt warns and leaves the table out, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3,3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4,4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm4');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT dolt_to_commit) FROM dolt_diff_t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C), and diffs pair rows across a primary key change by a key both versions hold, where Dolt warns and leaves the table out, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_val,to_pk,from_val,from_pk FROM dolt_diff_t;",
                    expected: Expected::Rows {
                        columns: &[Column("to_val", INT4), Column("to_pk", INT4), Column("from_val", INT4), Column("from_pk", INT4)],
                        rows: &[
                            &[T("4"), T("4"), Null, Null],
                            &[T("3"), T("3"), Null, Null],
                            &[T("2"), T("2"), Null, Null],
                            &[T("1"), T("1"), Null, Null],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("diffs pair rows across a primary key change by a key both versions hold, where Dolt warns and leaves the table out, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: test whether dolt_commit_diff correctly returns a diff whether there is or isn't a schema change",
            intent: "dolt_commit_diff_<table> warns across a primary key change and diffs commits after it.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm0');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2,2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3,3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm3');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4,4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm4');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT COUNT(DISTINCT dolt_to_commit) FROM dolt_commit_diff_t WHERE dolt_from_commit=dolt_hashof('HEAD~3') AND dolt_to_commit=dolt_hashof('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C), and diffs pair rows across a primary key change by a key both versions hold, where Dolt warns and leaves the table out, Appendix C \"diffs across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_val,to_pk,from_val,from_pk FROM dolt_commit_diff_t WHERE dolt_from_commit=dolt_hashof('HEAD~2') AND dolt_to_commit=dolt_hashof('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("to_val", INT4), Column("to_pk", INT4), Column("from_val", INT4), Column("from_pk", INT4)],
                        rows: &[
                            &[T("4"), T("4"), Null, Null],
                            &[T("3"), T("3"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: error dropping foreign key when used as a child in Fk relationship",
            intent: "A primary key a foreign key references can't be dropped.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE child(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE parent(pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent ADD CONSTRAINT myfk FOREIGN KEY (val) REFERENCES child (pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE child DROP CONSTRAINT child_pkey;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop constraint child_pkey on table child because other objects depend on it", detail: "constraint myfk on table parent depends on index child_pkey", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: dolt constraints verify works gracefully with schema violations",
            intent: "Verifying constraints after a primary key change finds no violations.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX myidx ON t (val);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE parent(pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE parent ADD CONSTRAINT fk FOREIGN KEY (pk) REFERENCES t (val);",
                    expected: Expected::Error(Diagnostic { code: "42830", message: "there is no unique constraint matching given keys for referenced table \"t\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_verify_constraints();",
                    expected: Expected::Rows {
                        columns: &[Column("violations", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt constraints verify checks the constraints, which dolt_verify_constraints() does")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: add/drop primary key in different order",
            intent: "A primary key comes back in another column order, and duplicates of the new key are refused.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int PRIMARY KEY, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (val, pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t2 (pk int PRIMARY KEY, val1 int, val2 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (1, 2, 2), (2, 2, 2);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 DROP CONSTRAINT t2_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t2 ADD PRIMARY KEY (val2, val1);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"t2_pkey\"", detail: "Key (val2, val1)=(2, 2) is duplicated.", schema: "public", table: "t2", constraint: "t2_pkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: add primary key on column that doesn't exist errors appropriately",
            intent: "A primary key on a missing column fails.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk1);",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"pk1\" of relation \"t\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: same primary key set in different order is detected and blocked on merge",
            intent: "A primary key on the same columns in another order is a different key that blocks a merge.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int, val int, PRIMARY KEY (pk, val));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (val, pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm2');",
                    expected: Expected::Ok,
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
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('test', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    val integer NOT NULL\n);\n\nALTER TABLE public.t OWNER TO postgres;\n\nALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (val, pk);"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    val integer NOT NULL\n);\n\nALTER TABLE public.t OWNER TO postgres;\n\nALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk, val);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'insert');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::Differs("dolt merge on the CLI keeps a conflicted merge in the working set, which dolt_allow_commit_conflicts lets dolt_merge do")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test', '-m', 'merge other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses the merge with \"cannot merge because table t has different primary keys\", a limitation its merge_schema.go TODO admits, where a merge takes the primary key one side changed to when the other side kept the base's key or made the same change, converting the other side's rows to it, Appendix C \"merges and cherry-picks across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('test');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("cannot fast forward from a to b. a is ahead of b already")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses the merge with \"cannot merge because table t has different primary keys\", a limitation its merge_schema.go TODO admits, where a merge takes the primary key one side changed to when the other side kept the base's key or made the same change, converting the other side's rows to it, Appendix C \"merges and cherry-picks across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'p';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("PRIMARY KEY (val, pk)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses the merge with \"cannot merge because table t has different primary keys\", a limitation its merge_schema.go TODO admits, where a merge takes the primary key one side changed to when the other side kept the base's key or made the same change, converting the other side's rows to it, Appendix C \"merges and cherry-picks across a primary key change\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: correct diff is returned even with a new added column",
            intent: "The schema diff shows a primary key moved onto a new column.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int, val int, PRIMARY KEY (pk, val));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('-b', 'test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD val2 int;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (val2, pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm2');",
                    expected: Expected::Ok,
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
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_create_statement, to_create_statement FROM dolt_schema_diff('test', 'WORKING', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("from_create_statement", TEXT), Column("to_create_statement", TEXT)],
                        rows: &[
                            &[T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    val integer NOT NULL,\n    val2 integer NOT NULL\n);\n\nALTER TABLE public.t OWNER TO postgres;\n\nALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (val2, pk);"), T("CREATE TABLE public.t (\n    pk integer NOT NULL,\n    val integer NOT NULL\n);\n\nALTER TABLE public.t OWNER TO postgres;\n\nALTER TABLE ONLY public.t\n    ADD CONSTRAINT t_pkey PRIMARY KEY (pk, val);")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt diff prints the schema diff, which dolt_schema_diff() returns as rows")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: column with duplicates throws an error when added as pk",
            intent: "A primary key can't be added on a column holding duplicates.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int, val int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1),(2,1);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'cm1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (val);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"t_pkey\"", detail: "Key (val)=(1) is duplicated.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: can't add a primary key on a column containing NULLs",
            intent: "A primary key can't be added on a column holding NULL.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int, c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (NULL, NULL);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY(pk);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "column \"pk\" of relation \"t\" contains null values", schema: "public", table: "t", column: "pk", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: create table with primary key adds not null constraint",
            intent: "A primary key column is NOT NULL.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 't'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: adding primary key also adds not null constraint",
            intent: "Adding a primary key makes its column NOT NULL.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 't'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: dropping primary key retains not null constraint",
            intent: "Dropping a primary key leaves its columns NOT NULL.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk1 int, pk2 int, c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD PRIMARY KEY (pk1, pk2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 't'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk1"), T("t"), T("f")],
                            &[T("pk2"), T("t"), T("f")],
                            &[T("c1"), T("f"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 't'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk1"), T("t"), T("f")],
                            &[T("pk2"), T("t"), T("f")],
                            &[T("c1"), T("f"), T("f")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: creating table with null and primary key column throws error",
            intent: "A column declared both NULL and PRIMARY KEY is refused.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int NULL PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "conflicting NULL/NOT NULL declarations for column \"pk\" of table \"t\"", position: 29, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: creating table with null and primary key column throws error again",
            intent: "A column declared NULL is refused as a table's primary key.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (pk int NULL, PRIMARY KEY(pk));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: can't add column with conflicting constraints",
            intent: "An added column declared both NULL and PRIMARY KEY is refused.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE t (c0 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ADD (pk int NULL PRIMARY KEY);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"(\"", position: 19, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: can add primary keys on db.table named tables",
            intent: "A primary key can be added to a table named with its database.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk int, c1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's database-qualified mydb.test names the current database's table test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD PRIMARY KEY(pk);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("t")],
                            &[T("c1"), T("f"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: can add and drop primary keys on keyless db.table named tables",
            intent: "A two-column primary key added and dropped on a database-named table leaves its columns NOT NULL.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk int, c1 VARCHAR(10), c2 bigint, c3 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's database-qualified mydb.test names the current database's table test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD PRIMARY KEY(pk, c1);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("t")],
                            &[T("c1"), T("t"), T("t")],
                            &[T("c2"), T("f"), T("f")],
                            &[T("c3"), T("f"), T("f")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP CONSTRAINT test_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("f")],
                            &[T("c1"), T("t"), T("f")],
                            &[T("c2"), T("f"), T("f")],
                            &[T("c3"), T("f"), T("f")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("f")],
                            &[T("c1"), T("t"), T("f")],
                            &[T("c2"), T("f"), T("f")],
                            &[T("c3"), T("f"), T("f")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary-key-changes: can drop and add multiple primary keys on db.table named tables",
            intent: "A primary key dropped and added on other columns of a database-named table.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test(pk int PRIMARY KEY, c1 int, c2 bigint, c3 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's database-qualified mydb.test names the current database's table test")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP CONSTRAINT test_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("f")],
                            &[T("c1"), T("f"), T("f")],
                            &[T("c2"), T("f"), T("f")],
                            &[T("c3"), T("f"), T("f")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE test ADD PRIMARY KEY(c1, c2);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("f")],
                            &[T("c1"), T("t"), T("t")],
                            &[T("c2"), T("t"), T("t")],
                            &[T("c3"), T("f"), T("f")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.attname, a.attnotnull, i.indisprimary IS TRUE FROM pg_attribute a LEFT JOIN pg_index i ON i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY (i.indkey) WHERE a.attrelid = 'test'::regclass AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attnotnull", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("pk"), T("t"), T("f")],
                            &[T("c1"), T("t"), T("t")],
                            &[T("c2"), T("t"), T("t")],
                            &[T("c3"), T("f"), T("f")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("DESCRIBE and SHOW CREATE TABLE are MySQL's, so pg_attribute and pg_index show each column's NOT NULL and primary key")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
