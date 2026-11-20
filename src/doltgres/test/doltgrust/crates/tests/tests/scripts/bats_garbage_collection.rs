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

//! Dolt's integration-tests/bats/garbage_collection.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_garbage_collection() {
    run_scripts(&[
        ScriptTest {
            name: r#"garbage_collection: gc on empty dir"#,
            intent: r#"Collecting a database with nothing in it succeeds, again, and shallowly."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc('-s');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: smoke test"#,
            intent: r#"Collections keep a table's rows and let later tables be made and read."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (1), (2), (3), (4), (5);"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"5"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_status;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T(r#"public.test"#), T(r#"f"#), T(r#"new table"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test2 (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test2 VALUES (1), (2), (3), (4), (5);"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"5"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM test2;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"5"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: call GC in sql script"#,
            intent: r#"dolt_gc runs after commits and a hard reset in the same session."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1), (2), (3);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'new table with three rows');"#,
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
                    query: r#"INSERT INTO t VALUES (11), (12), (13);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (21), (22), (23);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'new table with three rows');"#,
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
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk FROM t ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T(r#"1"#)],
                            &[T(r#"2"#)],
                            &[T(r#"3"#)],
                            &[T(r#"21"#)],
                            &[T(r#"22"#)],
                            &[T(r#"23"#)],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: blob types work after GC"#,
            intent: r#"Text values read back after a collection."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE t (pk INT PRIMARY KEY, val TEXT);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (1, 'one'), (2, 'two');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'added a table with blob encoding');"#,
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
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM t ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", TEXT)],
                        rows: &[
                            &[T(r#"1"#), T(r#"one"#)],
                            &[T(r#"2"#), T(r#"two"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: clone a remote"#,
            intent: r#"A clone keeps pulling from a remote after the original is collected."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_remote('add', 'test-remote', 'file://{NEWDIR:test-repo}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_remote", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (0), (1), (2);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'test commit');"#,
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
                    query: r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_clone('file://{NEWDIR:test-repo}');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_clone", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (10), (11), (12);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('test');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'test commit2');"#,
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
                    query: r#"SELECT * FROM dolt_push('test-remote', 'main');"#,
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#"0"#), Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT fast_forward, conflicts FROM dolt_pull();"#,
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8)],
                        rows: &[
                            &[T(r#"1"#), T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T(r#"6"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "test-repo",
                    database: "test-repo",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: leave committed and uncommitted data"#,
            intent: r#"A collection keeps committed rows and the working set's rows, not rows a hard reset dropped."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (1), (2), (3), (4), (5);"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'added values 1 - 5');"#,
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
                    query: r#"INSERT INTO test VALUES (6), (7), (8);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (11), (12), (13), (14), (15);"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"the test also measures .dolt/noms on disk, whose layout is Dolt's, so the port checks the data the collection keeps"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sum(pk) FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T(r#"80"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: leave conflicts"#,
            intent: r#"A collection keeps a merge's conflicts."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY, c0 TEXT);"#,
                r#"CREATE TABLE quiz (pk INT PRIMARY KEY, c0 TEXT);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'created tables test & quiz');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"INSERT INTO test VALUES (0, '10'), (1, '11'), (2, '12');"#,
                r#"SELECT dolt_commit('-am', 'added rows on main');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"INSERT INTO test VALUES (0, '20'), (1, '21'), (2, '22');"#,
                r#"SELECT dolt_commit('-am', 'added rows on other');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT base_pk, base_c0, our_pk, our_c0, their_pk, their_c0 FROM dolt_conflicts_test ORDER BY our_pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c0", TEXT), Column("our_pk", INT4), Column("our_c0", TEXT), Column("their_pk", INT4), Column("their_c0", TEXT)],
                        rows: &[
                            &[Null, Null, T(r#"0"#), T(r#"10"#), T(r#"0"#), T(r#"20"#)],
                            &[Null, Null, T(r#"1"#), T(r#"11"#), T(r#"1"#), T(r#"21"#)],
                            &[Null, Null, T(r#"2"#), T(r#"12"#), T(r#"2"#), T(r#"22"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT base_pk, base_c0, our_pk, our_c0, their_pk, their_c0 FROM dolt_conflicts_test ORDER BY our_pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c0", TEXT), Column("our_pk", INT4), Column("our_c0", TEXT), Column("their_pk", INT4), Column("their_c0", TEXT)],
                        rows: &[
                            &[Null, Null, T(r#"0"#), T(r#"10"#), T(r#"0"#), T(r#"20"#)],
                            &[Null, Null, T(r#"1"#), T(r#"11"#), T(r#"1"#), T(r#"21"#)],
                            &[Null, Null, T(r#"2"#), T(r#"12"#), T(r#"2"#), T(r#"22"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: leave constraint violations"#,
            intent: r#"A collection keeps a merge's constraint violations."#,
            set_up_script: &[
                r#"CREATE TABLE parent (pk INT PRIMARY KEY);"#,
                r#"CREATE TABLE child (pk INT PRIMARY KEY, fk INT, FOREIGN KEY (fk) REFERENCES parent (pk));"#,
                r#"INSERT INTO parent VALUES (1);"#,
                r#"SELECT dolt_commit('-Am', 'create tables and add parent');"#,
                r#"SELECT dolt_checkout('-b', 'other');"#,
                r#"INSERT INTO child VALUES (1, 1);"#,
                r#"SELECT dolt_commit('-am', 'add child');"#,
                r#"SELECT dolt_checkout('main');"#,
                r#"DELETE FROM parent WHERE pk = 1;"#,
                r#"SELECT dolt_commit('-am', 'remove parent');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET dolt_force_transaction_commit = 1;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a merge's constraint violations as dolt_force_transaction_commit does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, fk FROM dolt_constraint_violations_child;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("fk", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk, fk FROM dolt_constraint_violations_child;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("fk", INT4)],
                        rows: &[
                            &[T(r#"1"#), T(r#"1"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: leave merge commit"#,
            intent: r#"A merge in progress survives a collection and commits after its conflicts are resolved."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY, c0 TEXT);"#,
                r#"CREATE TABLE quiz (pk INT PRIMARY KEY, c0 TEXT);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'created tables test & quiz');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"INSERT INTO test VALUES (0, '10'), (1, '11'), (2, '12');"#,
                r#"SELECT dolt_commit('-am', 'added rows on main');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"INSERT INTO test VALUES (0, '20'), (1, '21'), (2, '22');"#,
                r#"SELECT dolt_commit('-am', 'added rows on other');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', '.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"dolt conflicts resolve is a CLI command, so the port calls dolt_conflicts_resolve"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'resolved conflicts with ours');"#,
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
                    query: r#"SELECT * FROM test ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"10"#)],
                            &[T(r#"1"#), T(r#"11"#)],
                            &[T(r#"2"#), T(r#"12"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: leave merge commit with stored procedure"#,
            intent: r#"A merge in progress survives a collection and commits after dolt_conflicts_resolve."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY, c0 TEXT);"#,
                r#"CREATE TABLE quiz (pk INT PRIMARY KEY, c0 TEXT);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'created tables test & quiz');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"INSERT INTO test VALUES (0, '10'), (1, '11'), (2, '12');"#,
                r#"SELECT dolt_commit('-am', 'added rows on main');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"INSERT INTO test VALUES (0, '20'), (1, '21'), (2, '22');"#,
                r#"SELECT dolt_commit('-am', 'added rows on other');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--ours', '.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'resolved conflicts with ours');"#,
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
                    query: r#"SELECT * FROM test ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"10"#)],
                            &[T(r#"1"#), T(r#"11"#)],
                            &[T(r#"2"#), T(r#"12"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: leave working pre-merge"#,
            intent: r#"Aborting a merge after a collection restores the working set from before the merge."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk INT PRIMARY KEY, c0 TEXT);"#,
                r#"CREATE TABLE quiz (pk INT PRIMARY KEY, c0 TEXT);"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'created tables test & quiz');"#,
                r#"SELECT dolt_branch('other');"#,
                r#"INSERT INTO test VALUES (0, '10'), (1, '11'), (2, '12');"#,
                r#"SELECT dolt_commit('-am', 'added rows on main');"#,
                r#"SELECT dolt_checkout('other');"#,
                r#"INSERT INTO test VALUES (0, '20'), (1, '21'), (2, '22');"#,
                r#"SELECT dolt_commit('-am', 'added rows on other');"#,
                r#"SELECT dolt_checkout('main');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO quiz VALUES (9, '99');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET dolt_allow_commit_conflicts = 1;"#,
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly(r#"the CLI keeps a merge's conflicts as dolt_allow_commit_conflicts does"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('--abort');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"0"#), T(r#"merge aborted"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM test ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", TEXT)],
                        rows: &[
                            &[T(r#"0"#), T(r#"10"#)],
                            &[T(r#"1"#), T(r#"11"#)],
                            &[T(r#"2"#), T(r#"12"#)],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM quiz;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", TEXT)],
                        rows: &[
                            &[T(r#"9"#), T(r#"99"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: online gc"#,
            intent: r#"dolt_gc keeps committed rows and the working set's rows, not rows a hard reset dropped."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk INT PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (1), (2), (3), (4), (5);"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-Am', 'added values 1-5');"#,
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
                    query: r#"INSERT INTO test VALUES (6), (7), (8);"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_reset('--hard');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO test VALUES (11), (12), (13), (14), (15);"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly(r#"the test also measures .dolt/noms on disk, whose layout is Dolt's, so the port checks the data the collection keeps"#)),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT sum(pk) FROM test;"#,
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8)],
                        rows: &[
                            &[T(r#"80"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: dolt gc --archive-level not 0"#,
            intent: r#"--archive-level takes 0 or 1."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc('--archive-level', '1');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc('--archive-level', '2');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for archive-level: 2"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc('--archive-level', '-1');"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid value for archive-level: -1"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: conflicts against a collected virtual merge base can be read and resolved"#,
            intent: r#"Conflicts against a merge base that no ref names survive a collection and resolve."#,
            set_up_script: &[
                r#"CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));"#,
                r#"INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'c1');"#,
                r#"SELECT dolt_branch('x');"#,
                r#"SELECT dolt_branch('y');"#,
                r#"SELECT dolt_checkout('x');"#,
                r#"UPDATE t SET a = 'x';"#,
                r#"SELECT dolt_commit('-am', 'x1');"#,
                r#"SELECT dolt_checkout('y');"#,
                r#"UPDATE t SET a = 'y' WHERE id = 1;"#,
                r#"UPDATE t SET b = 'y' WHERE id = 2;"#,
                r#"SELECT dolt_commit('-am', 'y1');"#,
                r#"SELECT dolt_branch('y1');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SELECT dolt_merge('x', '-m', 'y2');"#,
                r#"SELECT dolt_conflicts_resolve('--ours', 't');"#,
                r#"UPDATE t SET a = 'y wins' WHERE id = 1;"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'y2');"#,
                r#"SELECT dolt_checkout('x');"#,
                r#"SELECT dolt_merge('y1', '-m', 'x2');"#,
                r#"SELECT dolt_conflicts_resolve('--ours', 't');"#,
                r#"UPDATE t SET a = 'x wins' WHERE id = 1;"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'x2');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('y', '-m', 'merge y into x');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT base_a, our_a, their_a FROM dolt_conflicts_t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T(r#"seed"#), T(r#"x wins"#), T(r#"y wins"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT base_a, our_a, their_a FROM dolt_conflicts_t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T(r#"seed"#), T(r#"x wins"#), T(r#"y wins"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_conflicts_resolve('--theirs', 't');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_conflicts_resolve", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-am', 'resolve merge of y');"#,
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
                    query: r#"SELECT * FROM t ORDER BY id;"#,
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T(r#"1"#), T(r#"y wins"#), T(r#"seed"#)],
                            &[T(r#"2"#), T(r#"x"#), T(r#"y"#)],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"garbage_collection: committed conflicts against a collected virtual merge base can be read"#,
            intent: r#"Committed conflicts against a merge base that no ref names survive a collection."#,
            set_up_script: &[
                r#"CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));"#,
                r#"INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'c1');"#,
                r#"SELECT dolt_branch('x');"#,
                r#"SELECT dolt_branch('y');"#,
                r#"SELECT dolt_checkout('x');"#,
                r#"UPDATE t SET a = 'x';"#,
                r#"SELECT dolt_commit('-am', 'x1');"#,
                r#"SELECT dolt_checkout('y');"#,
                r#"UPDATE t SET a = 'y' WHERE id = 1;"#,
                r#"UPDATE t SET b = 'y' WHERE id = 2;"#,
                r#"SELECT dolt_commit('-am', 'y1');"#,
                r#"SELECT dolt_branch('y1');"#,
                r#"SET dolt_allow_commit_conflicts = 1;"#,
                r#"SELECT dolt_merge('x', '-m', 'y2');"#,
                r#"SELECT dolt_conflicts_resolve('--ours', 't');"#,
                r#"UPDATE t SET a = 'y wins' WHERE id = 1;"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'y2');"#,
                r#"SELECT dolt_checkout('x');"#,
                r#"SELECT dolt_merge('y1', '-m', 'x2');"#,
                r#"SELECT dolt_conflicts_resolve('--ours', 't');"#,
                r#"UPDATE t SET a = 'x wins' WHERE id = 1;"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'x2');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('y', '-m', 'merge y into x');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(r#""#), T(r#"0"#), T(r#"1"#), T(r#"conflicts found"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: r#"merge has unresolved conflicts or constraint violations"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('--force', '-am', 'commit with conflicts');"#,
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
                    query: r#"SELECT dolt_gc();"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_gc", INT8)],
                        rows: &[
                            &[T(r#"0"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT base_a, our_a, their_a FROM dolt_conflicts_t;"#,
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T(r#"seed"#), T(r#"x wins"#), T(r#"y wins"#)],
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
