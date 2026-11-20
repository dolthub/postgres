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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_merge_multiple_bases.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_merge_multiple_bases_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "schema branch: merge main into feature",
            intent: "Two branches that each merged a schema branch have two merge bases, and merging them keeps both sides' rows under the shared schema.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('schema');",
                "INSERT INTO t VALUES (1, 'seed'), (2, 'seed');",
                "SELECT dolt_commit('-am', 'c2');",
                "SELECT dolt_branch('feature');",
                "UPDATE t SET v = 'edited on main' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-am', 'c3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t VALUES (50, 'feature');",
                "SELECT dolt_commit('-am', 'f1');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN n VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "ALTER TABLE t ADD COLUMN m VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm');",
                "SELECT * FROM dolt_checkout('feature');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f');",
                "SELECT * FROM dolt_checkout('feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge main');",
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
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("n", VARCHAR), Column("m", VARCHAR)],
                        rows: &[
                            &[T("1"), T("edited on main"), Null, Null],
                            &[T("50"), T("feature"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema branch: merge feature into main",
            intent: "Two branches that each merged a schema branch have two merge bases, and merging them keeps both sides' rows under the shared schema.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('schema');",
                "INSERT INTO t VALUES (1, 'seed'), (2, 'seed');",
                "SELECT dolt_commit('-am', 'c2');",
                "SELECT dolt_branch('feature');",
                "UPDATE t SET v = 'edited on main' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-am', 'c3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t VALUES (50, 'feature');",
                "SELECT dolt_commit('-am', 'f1');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN n VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "ALTER TABLE t ADD COLUMN m VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm');",
                "SELECT * FROM dolt_checkout('feature');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature', '-m', 'merge feature');",
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
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("n", VARCHAR), Column("m", VARCHAR)],
                        rows: &[
                            &[T("1"), T("edited on main"), Null, Null],
                            &[T("50"), T("feature"), Null, Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema branch, fork point as tall as the schema tip: merge main into feature",
            intent: "Merge bases of equal height merge cleanly when the fork point is as tall as the schema tip.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('schema');",
                "INSERT INTO t VALUES (1, 'seed'), (2, 'seed');",
                "SELECT dolt_commit('-am', 'c2');",
                "INSERT INTO t VALUES (3, 'seed');",
                "SELECT dolt_commit('-am', 'c2-3');",
                "SELECT dolt_branch('feature');",
                "UPDATE t SET v = 'edited on main' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-am', 'c3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t VALUES (50, 'feature');",
                "SELECT dolt_commit('-am', 'f1');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN n VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "ALTER TABLE t ADD COLUMN m VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm');",
                "SELECT * FROM dolt_checkout('feature');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f');",
                "SELECT * FROM dolt_checkout('feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge main');",
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
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("n", VARCHAR), Column("m", VARCHAR)],
                        rows: &[
                            &[T("1"), T("edited on main"), Null, Null],
                            &[T("3"), T("seed"), Null, Null],
                            &[T("50"), T("feature"), Null, Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema branch, fork point taller than the schema tip: merge feature into main",
            intent: "Merge bases of different heights merge cleanly when the fork point is taller than the schema tip.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('schema');",
                "INSERT INTO t VALUES (1, 'seed'), (2, 'seed');",
                "SELECT dolt_commit('-am', 'c2');",
                "INSERT INTO t VALUES (3, 'seed');",
                "SELECT dolt_commit('-am', 'c2-3');",
                "INSERT INTO t VALUES (4, 'seed');",
                "SELECT dolt_commit('-am', 'c2-4');",
                "SELECT dolt_branch('feature');",
                "UPDATE t SET v = 'edited on main' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-am', 'c3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t VALUES (50, 'feature');",
                "SELECT dolt_commit('-am', 'f1');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN n VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "ALTER TABLE t ADD COLUMN m VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm');",
                "SELECT * FROM dolt_checkout('feature');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature', '-m', 'merge feature');",
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
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("n", VARCHAR), Column("m", VARCHAR)],
                        rows: &[
                            &[T("1"), T("edited on main"), Null, Null],
                            &[T("3"), T("seed"), Null, Null],
                            &[T("4"), T("seed"), Null, Null],
                            &[T("50"), T("feature"), Null, Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema branch, schema tip older than the fork point: merge main into feature",
            intent: "The merge bases are merged oldest first by committer date, which a schema tip older than the fork point reorders, with the same clean result.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('schema');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN n VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "ALTER TABLE t ADD COLUMN m VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 'seed'), (2, 'seed');",
                "SELECT dolt_commit('-am', 'c2');",
                "INSERT INTO t VALUES (3, 'seed');",
                "SELECT dolt_commit('-am', 'c2-3');",
                "INSERT INTO t VALUES (4, 'seed');",
                "SELECT dolt_commit('-am', 'c2-4');",
                "SELECT dolt_branch('feature');",
                "UPDATE t SET v = 'edited on main' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-am', 'c3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t VALUES (50, 'feature');",
                "SELECT dolt_commit('-am', 'f1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm');",
                "SELECT * FROM dolt_checkout('feature');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f');",
                "SELECT * FROM dolt_checkout('feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge main');",
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
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("n", VARCHAR), Column("m", VARCHAR)],
                        rows: &[
                            &[T("1"), T("edited on main"), Null, Null],
                            &[T("3"), T("seed"), Null, Null],
                            &[T("4"), T("seed"), Null, Null],
                            &[T("50"), T("feature"), Null, Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "long-running schema branch: merge main into feature",
            intent: "Six cycles of a schema branch merged into two long-running branches give many merge bases, which merge cleanly.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100), w VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c0');",
                "SELECT dolt_branch('schema');",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed'), (3, 'seed', 'seed');",
                "SELECT dolt_commit('-am', 'c1');",
                "SELECT dolt_branch('feature');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x1 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (101, 'main');",
                "UPDATE t SET v = 'main 1' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm1');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (201, 'feature');",
                "SELECT dolt_commit('-am', 'feat1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f1');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x2 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (102, 'main');",
                "UPDATE t SET v = 'main 2' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm2');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (202, 'feature');",
                "SELECT dolt_commit('-am', 'feat2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f2');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x3 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's3');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (103, 'main');",
                "UPDATE t SET v = 'main 3' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (203, 'feature');",
                "SELECT dolt_commit('-am', 'feat3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f3');",
                "SELECT * FROM dolt_merge('main', '-m', 'sync');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x4 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's4');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (104, 'main');",
                "UPDATE t SET v = 'main 4' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm4');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (204, 'feature');",
                "SELECT dolt_commit('-am', 'feat4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f4');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x5 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's5');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (105, 'main');",
                "UPDATE t SET v = 'main 5' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-am', 'main5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm5');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (205, 'feature');",
                "SELECT dolt_commit('-am', 'feat5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f5');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x6 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's6');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (106, 'main');",
                "UPDATE t SET v = 'main 6' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm6');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (206, 'feature');",
                "SELECT dolt_commit('-am', 'feat6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f6');",
                "SELECT * FROM dolt_checkout('feature');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge main');",
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
                    query: "SELECT id, v, w FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("w", VARCHAR)],
                        rows: &[
                            &[T("1"), T("main 6"), T("seed")],
                            &[T("3"), T("seed"), T("seed")],
                            &[T("101"), T("main"), Null],
                            &[T("102"), T("main"), Null],
                            &[T("103"), T("main"), Null],
                            &[T("104"), T("main"), Null],
                            &[T("105"), T("main"), Null],
                            &[T("106"), T("main"), Null],
                            &[T("201"), T("feature"), Null],
                            &[T("202"), T("feature"), Null],
                            &[T("203"), T("feature"), Null],
                            &[T("204"), T("feature"), Null],
                            &[T("205"), T("feature"), Null],
                            &[T("206"), T("feature"), Null],
                        ],
                        tag: "SELECT 14",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM information_schema.columns WHERE table_schema = CURRENT_SCHEMA() AND table_name = 't';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "long-running schema branch: merge feature into main",
            intent: "Six cycles of a schema branch merged into two long-running branches give many merge bases, which merge cleanly.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100), w VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c0');",
                "SELECT dolt_branch('schema');",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed'), (3, 'seed', 'seed');",
                "SELECT dolt_commit('-am', 'c1');",
                "SELECT dolt_branch('feature');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x1 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (101, 'main');",
                "UPDATE t SET v = 'main 1' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm1');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (201, 'feature');",
                "SELECT dolt_commit('-am', 'feat1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f1');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x2 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (102, 'main');",
                "UPDATE t SET v = 'main 2' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm2');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (202, 'feature');",
                "SELECT dolt_commit('-am', 'feat2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f2');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x3 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's3');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (103, 'main');",
                "UPDATE t SET v = 'main 3' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (203, 'feature');",
                "SELECT dolt_commit('-am', 'feat3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f3');",
                "SELECT * FROM dolt_merge('main', '-m', 'sync');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x4 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's4');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (104, 'main');",
                "UPDATE t SET v = 'main 4' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm4');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (204, 'feature');",
                "SELECT dolt_commit('-am', 'feat4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f4');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x5 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's5');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (105, 'main');",
                "UPDATE t SET v = 'main 5' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-am', 'main5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm5');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (205, 'feature');",
                "SELECT dolt_commit('-am', 'feat5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f5');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x6 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's6');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (106, 'main');",
                "UPDATE t SET v = 'main 6' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm6');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (206, 'feature');",
                "SELECT dolt_commit('-am', 'feat6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f6');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature', '-m', 'merge feature');",
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
                    query: "SELECT id, v, w FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("w", VARCHAR)],
                        rows: &[
                            &[T("1"), T("main 6"), T("seed")],
                            &[T("3"), T("seed"), T("seed")],
                            &[T("101"), T("main"), Null],
                            &[T("102"), T("main"), Null],
                            &[T("103"), T("main"), Null],
                            &[T("104"), T("main"), Null],
                            &[T("105"), T("main"), Null],
                            &[T("106"), T("main"), Null],
                            &[T("201"), T("feature"), Null],
                            &[T("202"), T("feature"), Null],
                            &[T("203"), T("feature"), Null],
                            &[T("204"), T("feature"), Null],
                            &[T("205"), T("feature"), Null],
                            &[T("206"), T("feature"), Null],
                        ],
                        tag: "SELECT 14",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "long-running schema branch with a conflict: merge main into feature",
            intent: "A row edited on both long-running branches conflicts against its value at their last merge, while another row merges column by column.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100), w VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c0');",
                "SELECT dolt_branch('schema');",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed'), (3, 'seed', 'seed');",
                "SELECT dolt_commit('-am', 'c1');",
                "SELECT dolt_branch('feature');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x1 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (101, 'main');",
                "UPDATE t SET v = 'main 1' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm1');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (201, 'feature');",
                "SELECT dolt_commit('-am', 'feat1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f1');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x2 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (102, 'main');",
                "UPDATE t SET v = 'main 2' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm2');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (202, 'feature');",
                "SELECT dolt_commit('-am', 'feat2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f2');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x3 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's3');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (103, 'main');",
                "UPDATE t SET v = 'main 3' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (203, 'feature');",
                "SELECT dolt_commit('-am', 'feat3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f3');",
                "SELECT * FROM dolt_merge('main', '-m', 'sync');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x4 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's4');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (104, 'main');",
                "UPDATE t SET v = 'main 4' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm4');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (204, 'feature');",
                "SELECT dolt_commit('-am', 'feat4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f4');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x5 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's5');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (105, 'main');",
                "UPDATE t SET v = 'main 5' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "UPDATE t SET v = 'main' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'main5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm5');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (205, 'feature');",
                "UPDATE t SET v = 'feature 5' WHERE id = 1;",
                "UPDATE t SET w = 'feature' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'feat5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f5');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x6 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's6');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (106, 'main');",
                "UPDATE t SET v = 'main 6' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm6');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (206, 'feature');",
                "SELECT dolt_commit('-am', 'feat6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f6');",
                "SELECT * FROM dolt_checkout('feature');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge main');",
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
                    query: "SELECT base_id, base_v, base_w, our_v, our_w, their_v, their_w FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_v", VARCHAR), Column("base_w", VARCHAR), Column("our_v", VARCHAR), Column("our_w", VARCHAR), Column("their_v", VARCHAR), Column("their_w", VARCHAR)],
                        rows: &[
                            &[T("1"), T("main 3"), T("seed"), T("feature 5"), T("seed"), T("main 6"), T("seed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v, w FROM t WHERE id <> 1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("w", VARCHAR)],
                        rows: &[
                            &[T("3"), T("main"), T("feature")],
                            &[T("101"), T("main"), Null],
                            &[T("102"), T("main"), Null],
                            &[T("103"), T("main"), Null],
                            &[T("104"), T("main"), Null],
                            &[T("105"), T("main"), Null],
                            &[T("106"), T("main"), Null],
                            &[T("201"), T("feature"), Null],
                            &[T("202"), T("feature"), Null],
                            &[T("203"), T("feature"), Null],
                            &[T("204"), T("feature"), Null],
                            &[T("205"), T("feature"), Null],
                            &[T("206"), T("feature"), Null],
                        ],
                        tag: "SELECT 13",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "long-running schema branch with a conflict: merge feature into main",
            intent: "A row edited on both long-running branches conflicts against its value at their last merge, while another row merges column by column.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100), w VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c0');",
                "SELECT dolt_branch('schema');",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed'), (3, 'seed', 'seed');",
                "SELECT dolt_commit('-am', 'c1');",
                "SELECT dolt_branch('feature');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x1 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (101, 'main');",
                "UPDATE t SET v = 'main 1' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm1');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (201, 'feature');",
                "SELECT dolt_commit('-am', 'feat1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f1');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x2 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (102, 'main');",
                "UPDATE t SET v = 'main 2' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm2');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (202, 'feature');",
                "SELECT dolt_commit('-am', 'feat2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f2');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x3 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's3');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (103, 'main');",
                "UPDATE t SET v = 'main 3' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (203, 'feature');",
                "SELECT dolt_commit('-am', 'feat3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f3');",
                "SELECT * FROM dolt_merge('main', '-m', 'sync');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x4 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's4');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (104, 'main');",
                "UPDATE t SET v = 'main 4' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm4');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (204, 'feature');",
                "SELECT dolt_commit('-am', 'feat4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f4');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x5 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's5');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (105, 'main');",
                "UPDATE t SET v = 'main 5' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "UPDATE t SET v = 'main' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'main5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm5');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (205, 'feature');",
                "UPDATE t SET v = 'feature 5' WHERE id = 1;",
                "UPDATE t SET w = 'feature' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'feat5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f5');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x6 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's6');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (106, 'main');",
                "UPDATE t SET v = 'main 6' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm6');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (206, 'feature');",
                "SELECT dolt_commit('-am', 'feat6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f6');",
                "SELECT * FROM dolt_checkout('main');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature', '-m', 'merge feature');",
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
                    query: "SELECT base_id, base_v, base_w, our_v, our_w, their_v, their_w FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_v", VARCHAR), Column("base_w", VARCHAR), Column("our_v", VARCHAR), Column("our_w", VARCHAR), Column("their_v", VARCHAR), Column("their_w", VARCHAR)],
                        rows: &[
                            &[T("1"), T("main 3"), T("seed"), T("main 6"), T("seed"), T("feature 5"), T("seed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v, w FROM t WHERE id <> 1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("w", VARCHAR)],
                        rows: &[
                            &[T("3"), T("main"), T("feature")],
                            &[T("101"), T("main"), Null],
                            &[T("102"), T("main"), Null],
                            &[T("103"), T("main"), Null],
                            &[T("104"), T("main"), Null],
                            &[T("105"), T("main"), Null],
                            &[T("106"), T("main"), Null],
                            &[T("201"), T("feature"), Null],
                            &[T("202"), T("feature"), Null],
                            &[T("203"), T("feature"), Null],
                            &[T("204"), T("feature"), Null],
                            &[T("205"), T("feature"), Null],
                            &[T("206"), T("feature"), Null],
                        ],
                        tag: "SELECT 13",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases, resolved the same way: merge y into x",
            intent: "Merge bases that conflicted, resolved the same way on both tips, merge cleanly to that resolution.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved"), T("seed")],
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases, resolved differently: merge y into x",
            intent: "Merge bases that conflicted, resolved differently on the tips, conflict again with the merge bases' own base as the base value.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_id, base_a, base_b, our_a, our_b, their_a, their_b FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("base_b", VARCHAR), Column("our_a", VARCHAR), Column("our_b", VARCHAR), Column("their_a", VARCHAR), Column("their_b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), T("seed"), T("x wins"), T("seed"), T("y wins"), T("seed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t WHERE id = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases, then an edit to a row they merged cleanly: merge y into x",
            intent: "A row the merge bases merged cleanly takes a later edit without conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x later' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved"), T("seed")],
                            &[T("2"), T("x later"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "three merge bases: merge m2 into m1",
            intent: "Three merge bases are merged together oldest first, their own conflict included, before the tips merge.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed'), (3, 'seed', 'seed'), (4, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('p1');",
                "SELECT dolt_branch('p2');",
                "SELECT dolt_branch('p3');",
                "SELECT dolt_branch('m1');",
                "SELECT dolt_branch('m2');",
                "SELECT * FROM dolt_checkout('p1');",
                "UPDATE t SET a = 'p1' WHERE id IN (1, 2);",
                "SELECT dolt_commit('-Am', 'p1');",
                "SELECT * FROM dolt_checkout('p2');",
                "UPDATE t SET a = 'p2' WHERE id IN (1, 3);",
                "SELECT dolt_commit('-Am', 'p2');",
                "SELECT * FROM dolt_checkout('p3');",
                "UPDATE t SET b = 'p3' WHERE id = 4;",
                "SELECT dolt_commit('-Am', 'p3');",
                "SELECT * FROM dolt_checkout('m1');",
                "INSERT INTO t VALUES (10, 'm1', NULL);",
                "SELECT dolt_commit('-Am', 'm1start');",
                "SELECT dolt_branch('m1start');",
                "SELECT * FROM dolt_checkout('m1');",
                "SELECT * FROM dolt_merge('p1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'm1 merges p1');",
                "SELECT * FROM dolt_checkout('m1');",
                "SELECT * FROM dolt_merge('p2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'm1 merges p2');",
                "SELECT * FROM dolt_checkout('m1');",
                "SELECT * FROM dolt_merge('p3', '--no-commit');",
                "SELECT dolt_commit('-Am', 'm1 merges p3');",
                "SELECT * FROM dolt_checkout('m1');",
                "UPDATE t SET b = 'm1' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'm1later');",
                "SELECT dolt_branch('m1later');",
                "SELECT * FROM dolt_checkout('m2');",
                "INSERT INTO t VALUES (20, 'm2', NULL);",
                "SELECT dolt_commit('-Am', 'm2start');",
                "SELECT dolt_branch('m2start');",
                "SELECT * FROM dolt_checkout('m2');",
                "SELECT * FROM dolt_merge('p3', '--no-commit');",
                "SELECT dolt_commit('-Am', 'm2 merges p3');",
                "SELECT * FROM dolt_checkout('m2');",
                "SELECT * FROM dolt_merge('p2', '--no-commit');",
                "SELECT dolt_commit('-Am', 'm2 merges p2');",
                "SELECT * FROM dolt_checkout('m2');",
                "SELECT * FROM dolt_merge('p1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'm2 merges p1');",
                "SELECT * FROM dolt_checkout('m2');",
                "UPDATE t SET b = 'm2' WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'm2later');",
                "SELECT dolt_branch('m2later');",
                "SELECT * FROM dolt_checkout('m1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('m2', '-m', 'merge m2');",
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
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved"), T("seed")],
                            &[T("2"), T("p1"), T("m1")],
                            &[T("3"), T("p2"), T("m2")],
                            &[T("4"), T("seed"), T("p3")],
                            &[T("10"), T("m1"), Null],
                            &[T("20"), T("m2"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that conflict on a deleted row, both deleting it: merge y into x",
            intent: "A row whose deletion conflicted between the merge bases merges cleanly when both tips delete it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "DELETE FROM t WHERE id = 1;",
                "UPDATE t SET a = 'x' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that conflict on a deleted row, both keeping it: merge y into x",
            intent: "A row whose deletion conflicted between the merge bases merges cleanly when both tips keep it the same way.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "DELETE FROM t WHERE id = 1;",
                "UPDATE t SET a = 'x' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (1, 'y', 'seed') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (1, 'y', 'seed') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("y"), T("seed")],
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that conflict on a deleted row, resolved differently: merge y into x",
            intent: "A row one tip deleted and the other kept, after the merge bases conflicted on it, conflicts, as in git.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "DELETE FROM t WHERE id = 1;",
                "UPDATE t SET a = 'x' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (1, 'y', 'seed') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_id, base_a, our_id, their_id, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("our_id", INT4), Column("their_id", INT4), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), Null, T("1"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases with a secondary index: merge y into x",
            intent: "A clean merge over conflicting merge bases keeps a secondary index that matches the merged rows.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100)); CREATE INDEX idx_b ON t (b);",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x later' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved"), T("seed")],
                            &[T("2"), T("x later"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t  WHERE b = 'y';",
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
                    query: "SELECT id FROM t  WHERE b = 'seed';",
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
        ScriptTest {
            name: "conflicting merge bases, one side dropping the table: merge y into x",
            intent: "Dropping a table on one tip conflicts when the other tip changed a row from the conflicted merge bases, even back to the old value.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "DROP TABLE t;",
                "SELECT dolt_commit('-am', 'x3');",
            ],
            assertions: &[
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
                    query: "SELECT table_name, our_schema, description FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("our_schema", TEXT), Column("description", TEXT)],
                        rows: &[
                            &[T("public.t"), T("<deleted>"), T("cannot merge a table deletion with data modification")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name, status FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("schema conflict")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases with different schemas: merge y into x",
            intent: "Merge bases with different schemas merge row by row, so a row deleted on one tip stays deleted and a row conflicted between them conflicts again.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "UPDATE t SET a = 'x' WHERE id = 1;",
                "INSERT INTO t VALUES (3, 'x', 'x', 'x');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "DELETE FROM t WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_id, base_a, our_a, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), T("x wins"), T("seed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR), Column("c", VARCHAR)],
                        rows: &[
                            &[T("1"), T("x wins"), T("seed"), Null],
                            &[T("2"), T("seed"), T("seed"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases, resolved the same way, then a type change: merge y into x",
            intent: "Values equal in the merged type don't conflict when one tip later changes the column's type.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t ALTER COLUMN a TYPE TEXT;",
                "SELECT dolt_commit('-am', 'y3');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("a", TEXT), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved"), T("seed")],
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases with merged JSON documents, with @@dolt_dont_merge_json: merge y into x",
            intent: "The virtual merge base merges JSON documents whatever dolt_dont_merge_json is, so only the tip that changed the document changes it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, j JSON);",
                "INSERT INTO t VALUES (1, '{}');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET j = '{\"x\": 1}';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET j = '{\"y\": 1}';",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET j = '{\"x\": 2, \"y\": 1}';",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
                "SET dolt_dont_merge_json = 1;",
            ],
            assertions: &[
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
                    query: "SELECT j FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("j", JSON)],
                        rows: &[
                            &[T("{\"x\": 2, \"y\": 1}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases with a virtual generated column first, resolved differently: merge y into x",
            intent: "A virtual generated column before the conflicted column doesn't shift which stored value conflicts.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, g int GENERATED ALWAYS AS (id * 2) VIRTUAL, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_a, base_b, our_a, our_b, their_a, their_b FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("base_b", VARCHAR), Column("our_a", VARCHAR), Column("our_b", VARCHAR), Column("their_a", VARCHAR), Column("their_b", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("seed"), T("x wins"), T("seed"), T("y wins"), T("seed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t WHERE id = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("2"), T("4"), T("x"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases with a virtual generated column first, resolved the same way: merge y into x",
            intent: "A virtual generated column before the conflicted column doesn't stop the tips' identical resolutions from merging cleanly.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, g int GENERATED ALWAYS AS (id * 2) VIRTUAL, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x later' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("g", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("2"), T("resolved"), T("seed")],
                            &[T("2"), T("4"), T("x later"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases with a stored generated column, then an edit to its source: merge y into x",
            intent: "A stored generated value is computed, not merged, so a later edit to its other source doesn't conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100), s VARCHAR(201) GENERATED ALWAYS AS (a || '-' || b) STORED);",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET b = 'x later' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'x3');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR), Column("s", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved"), T("x later"), T("resolved-x later")],
                            &[T("2"), T("x"), T("y"), T("x-y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases, resolved the same way, then a column move: merge y into x",
            intent: "A tip that moves the resolved column to another position still merges cleanly by column identity.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t ALTER COLUMN a TYPE VARCHAR(100) AFTER b;",
                "SELECT dolt_commit('-am', 'y3');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT id, a, b FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved"), T("seed")],
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: Postgres can't move a column to another position"),
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases, one side resolved to the old value, then a type change: merge y into x",
            intent: "A tip resolved back to the old value still conflicts with the other tip's value after a type change.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t ALTER COLUMN a TYPE TEXT;",
                "SELECT dolt_commit('-am', 'y3');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_a, our_a, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("our_a", TEXT), Column("their_a", TEXT)],
                        rows: &[
                            &[T("seed"), T("seed"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases, resolved differently, then a drop of the conflicted column: merge y into x",
            intent: "Dropping a column whose value the tips resolved differently conflicts on the row, as with one merge base.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t DROP COLUMN a;",
                "SELECT dolt_commit('-am', 'y3');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_a, base_b, our_b, dolt_our_diff_type, their_b, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("base_b", VARCHAR), Column("our_b", VARCHAR), Column("dolt_our_diff_type", TEXT), Column("their_b", VARCHAR), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("seed"), T("seed"), T("seed"), T("modified"), T("seed"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that violate a unique key together: merge y into x",
            intent: "Merge bases that together violate a unique key build a virtual merge base whose index still builds, and the tips' fixes merge cleanly.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), CONSTRAINT ua UNIQUE (a));",
                "INSERT INTO t VALUES (1, 'seed 1'), (2, 'seed 2');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'u' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'u' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "UPDATE t SET a = 'y2' WHERE id = 2;",
                "DELETE FROM dolt_constraint_violations_t;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "UPDATE t SET a = 'x2' WHERE id = 1;",
                "DELETE FROM dolt_constraint_violations_t;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("a", VARCHAR)],
                        rows: &[
                            &[T("1"), T("x2")],
                            &[T("2"), T("y2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t  WHERE a = 'y2';",
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
            name: "merge bases that violate a unique key together, each side moving the other row: merge y into x",
            intent: "When each tip moves the other's row away from the duplicated value, the merge takes both changes and neither row keeps it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), CONSTRAINT ua UNIQUE (a));",
                "INSERT INTO t VALUES (1, 'seed 1'), (2, 'seed 2');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'u' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'u' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "UPDATE t SET a = 'y2' WHERE id = 1;",
                "DELETE FROM dolt_constraint_violations_t;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "UPDATE t SET a = 'x2' WHERE id = 2;",
                "DELETE FROM dolt_constraint_violations_t;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("a", VARCHAR)],
                        rows: &[
                            &[T("1"), T("y2")],
                            &[T("2"), T("x2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that violate a check constraint together: merge y into x",
            intent: "A row that violates a check in the virtual merge base merges cleanly when both tips fix it the same way.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100), CHECK (a <> b));",
                "INSERT INTO t VALUES (1, 'seed a', 'seed b');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'z' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET b = 'z' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "UPDATE t SET b = 'fixed' WHERE id = 1;",
                "DELETE FROM dolt_constraint_violations_t;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "UPDATE t SET b = 'fixed' WHERE id = 1;",
                "DELETE FROM dolt_constraint_violations_t;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("z"), T("fixed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that violate a foreign key together: merge y into x",
            intent: "A foreign key the merge bases violate together is checked against the tips' merged rows, not the virtual merge base's.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE parent (id int PRIMARY KEY);",
                "CREATE TABLE child (id int PRIMARY KEY, pid int, FOREIGN KEY (pid) REFERENCES parent (id));",
                "INSERT INTO parent VALUES (1), (2);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "DELETE FROM parent WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "INSERT INTO child VALUES (10, 2);",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "DELETE FROM dolt_constraint_violations_child;",
                "DELETE FROM child WHERE id = 10;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "DELETE FROM dolt_constraint_violations_child;",
                "INSERT INTO parent VALUES (2);",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM parent ORDER BY id;",
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
                    query: "SELECT * FROM child;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("pid", INT4)],
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
            name: "merge bases with a conflict and a unique key violation: merge y into x",
            intent: "A row that both conflicts and violates a unique key between the merge bases conflicts again on the tips' changes.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100), CONSTRAINT ua UNIQUE (a));",
                "INSERT INTO t VALUES (1, 'seed 1', 'seed'), (2, 'seed 2', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'u', b = 'x' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'u' WHERE id = 2;",
                "UPDATE t SET b = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'fixed' WHERE id = 2;",
                "DELETE FROM dolt_constraint_violations_t;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'fixed' WHERE id = 2;",
                "DELETE FROM dolt_constraint_violations_t;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_id, base_a, base_b, our_a, our_b, their_a, their_b FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("base_b", VARCHAR), Column("our_a", VARCHAR), Column("our_b", VARCHAR), Column("their_a", VARCHAR), Column("their_b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("u"), T("seed"), T("u"), T("x"), T("seed 1"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t WHERE id = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("2"), T("fixed"), T("seed")],
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
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that add a row with different values, resolved the same way: merge y into x",
            intent: "A row the merge bases added differently merges cleanly when the tips agree on it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'same') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'same') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("a", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed")],
                            &[T("3"), T("same")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that add a row with different values, both deleting it: merge y into x",
            intent: "A row the merge bases added differently merges cleanly when both tips delete it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                        columns: &[Column("id", INT4), Column("a", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that create a table with different rows, resolved differently: merge y into x",
            intent: "A table both merge bases created with different rows has only their shared rows in the virtual merge base, so the tips' different rows conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "CREATE TABLE u (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO u VALUES (1, 'x'), (2, 'same');",
                "SELECT dolt_add('u');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "CREATE TABLE u (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO u VALUES (1, 'y'), (2, 'same');",
                "SELECT dolt_add('u');",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 'u');",
                "UPDATE u SET a = 'y2' WHERE id = 1;",
                "DELETE FROM u WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 'u');",
                "UPDATE u SET a = 'x2' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_id, base_a, our_id, our_a, their_id, their_a FROM dolt_conflicts_u;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("our_id", INT4), Column("our_a", VARCHAR), Column("their_id", INT4), Column("their_a", VARCHAR)],
                        rows: &[
                            &[Null, Null, T("1"), T("x2"), T("1"), T("y2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR)],
                        rows: &[
                            &[T("1"), T("x2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "three merge bases that conflict in different folds, tips disagreeing on the first conflicted cell: merge m2 into m1",
            intent: "With three merge bases, a cell conflicted in the first fold conflicts again when the tips disagree on it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed'), (3, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('p1');",
                "SELECT dolt_branch('p2');",
                "SELECT dolt_branch('p3');",
                "SELECT dolt_branch('m1');",
                "SELECT dolt_branch('m2');",
                "SELECT * FROM dolt_checkout('p1');",
                "UPDATE t SET a = 'p1' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'p1');",
                "SELECT * FROM dolt_checkout('p2');",
                "UPDATE t SET a = 'p2', b = 'p2' WHERE id = 1;",
                "UPDATE t SET b = 'p2' WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'p2');",
                "SELECT * FROM dolt_checkout('p3');",
                "UPDATE t SET b = 'p3' WHERE id = 1;",
                "UPDATE t SET a = 'p3' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'p3');",
                "SELECT * FROM dolt_checkout('m1');",
                "INSERT INTO t VALUES (10, 'm1', NULL);",
                "SELECT dolt_commit('-Am', 'm1start');",
                "SELECT dolt_branch('m1start');",
                "SELECT * FROM dolt_checkout('m1');",
                "SELECT * FROM dolt_merge('p1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'm1 merges p1');",
                "SELECT * FROM dolt_checkout('m1');",
                "SELECT * FROM dolt_merge('p2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "SELECT dolt_commit('-Am', 'm1 merges p2');",
                "SELECT * FROM dolt_checkout('m1');",
                "SELECT * FROM dolt_merge('p3', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "SELECT dolt_commit('-Am', 'm1 merges p3');",
                "SELECT * FROM dolt_checkout('m1');",
                "INSERT INTO t VALUES (1, 'm1', 'both') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'm1later');",
                "SELECT dolt_branch('m1later');",
                "SELECT * FROM dolt_checkout('m2');",
                "INSERT INTO t VALUES (20, 'm2', NULL);",
                "SELECT dolt_commit('-Am', 'm2start');",
                "SELECT dolt_branch('m2start');",
                "SELECT * FROM dolt_checkout('m2');",
                "SELECT * FROM dolt_merge('p3', '--no-commit');",
                "SELECT dolt_commit('-Am', 'm2 merges p3');",
                "SELECT * FROM dolt_checkout('m2');",
                "SELECT * FROM dolt_merge('p2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "SELECT dolt_commit('-Am', 'm2 merges p2');",
                "SELECT * FROM dolt_checkout('m2');",
                "SELECT * FROM dolt_merge('p1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "SELECT dolt_commit('-Am', 'm2 merges p1');",
                "SELECT * FROM dolt_checkout('m2');",
                "INSERT INTO t VALUES (1, 'seed', 'both') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "INSERT INTO t VALUES (2, 'seed', 'seed') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "SELECT dolt_commit('-Am', 'm2later');",
                "SELECT dolt_branch('m2later');",
                "SELECT * FROM dolt_checkout('m1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('m2', '-m', 'merge m2');",
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
                    query: "SELECT base_id, base_a, base_b, our_id, our_a, our_b, their_id, their_a, their_b FROM dolt_conflicts_t ORDER BY base_id;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("base_b", VARCHAR), Column("our_id", INT4), Column("our_a", VARCHAR), Column("our_b", VARCHAR), Column("their_id", INT4), Column("their_a", VARCHAR), Column("their_b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), T("seed"), T("1"), T("m1"), T("both"), T("1"), T("seed"), T("both")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t WHERE id <> 1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("3"), T("seed"), T("p2")],
                            &[T("10"), T("m1"), Null],
                            &[T("20"), T("m2"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "three merge bases that conflict in different folds, tips disagreeing on the second conflicted cell: merge m2 into m1",
            intent: "With three merge bases, a cell conflicted in the second fold conflicts again when the tips disagree on it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed'), (3, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('p1');",
                "SELECT dolt_branch('p2');",
                "SELECT dolt_branch('p3');",
                "SELECT dolt_branch('m1');",
                "SELECT dolt_branch('m2');",
                "SELECT * FROM dolt_checkout('p1');",
                "UPDATE t SET a = 'p1' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'p1');",
                "SELECT * FROM dolt_checkout('p2');",
                "UPDATE t SET a = 'p2', b = 'p2' WHERE id = 1;",
                "UPDATE t SET b = 'p2' WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'p2');",
                "SELECT * FROM dolt_checkout('p3');",
                "UPDATE t SET b = 'p3' WHERE id = 1;",
                "UPDATE t SET a = 'p3' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'p3');",
                "SELECT * FROM dolt_checkout('m1');",
                "INSERT INTO t VALUES (10, 'm1', NULL);",
                "SELECT dolt_commit('-Am', 'm1start');",
                "SELECT dolt_branch('m1start');",
                "SELECT * FROM dolt_checkout('m1');",
                "SELECT * FROM dolt_merge('p1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'm1 merges p1');",
                "SELECT * FROM dolt_checkout('m1');",
                "SELECT * FROM dolt_merge('p2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "SELECT dolt_commit('-Am', 'm1 merges p2');",
                "SELECT * FROM dolt_checkout('m1');",
                "SELECT * FROM dolt_merge('p3', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "SELECT dolt_commit('-Am', 'm1 merges p3');",
                "SELECT * FROM dolt_checkout('m1');",
                "INSERT INTO t VALUES (1, 'both', 'seed') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'm1later');",
                "SELECT dolt_branch('m1later');",
                "SELECT * FROM dolt_checkout('m2');",
                "INSERT INTO t VALUES (20, 'm2', NULL);",
                "SELECT dolt_commit('-Am', 'm2start');",
                "SELECT dolt_branch('m2start');",
                "SELECT * FROM dolt_checkout('m2');",
                "SELECT * FROM dolt_merge('p3', '--no-commit');",
                "SELECT dolt_commit('-Am', 'm2 merges p3');",
                "SELECT * FROM dolt_checkout('m2');",
                "SELECT * FROM dolt_merge('p2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "SELECT dolt_commit('-Am', 'm2 merges p2');",
                "SELECT * FROM dolt_checkout('m2');",
                "SELECT * FROM dolt_merge('p1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "SELECT dolt_commit('-Am', 'm2 merges p1');",
                "SELECT * FROM dolt_checkout('m2');",
                "INSERT INTO t VALUES (1, 'both', 'm2') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "INSERT INTO t VALUES (2, 'seed', 'seed') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "SELECT dolt_commit('-Am', 'm2later');",
                "SELECT dolt_branch('m2later');",
                "SELECT * FROM dolt_checkout('m1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('m2', '-m', 'merge m2');",
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
                    query: "SELECT base_id, base_a, base_b, our_id, our_a, our_b, their_id, their_a, their_b FROM dolt_conflicts_t ORDER BY base_id;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("base_b", VARCHAR), Column("our_id", INT4), Column("our_a", VARCHAR), Column("our_b", VARCHAR), Column("their_id", INT4), Column("their_a", VARCHAR), Column("their_b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), T("seed"), T("1"), T("both"), T("seed"), T("1"), T("both"), T("m2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases with a stored generated column, resolved differently: merge y into x",
            intent: "A stored generated column is computed in the virtual merge base from its merged sources, and the tips' different resolutions conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a int, b int, g int GENERATED ALWAYS AS (a + b) STORED); CREATE INDEX ib ON t (b); CREATE INDEX ig ON t (g);",
                "INSERT INTO t (id, a, b) VALUES (1, 0, 0);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 1, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 3 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 20, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 10, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_a, base_b, base_g, our_a, our_g, their_a, their_g FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", INT4), Column("base_b", INT4), Column("base_g", INT4), Column("our_a", INT4), Column("our_g", INT4), Column("their_a", INT4), Column("their_g", INT4)],
                        rows: &[
                            &[T("0"), T("1"), T("1"), T("10"), T("11"), T("20"), T("21")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that conflict on a row and on its JSON document, then a change to the document: merge y into x",
            intent: "A JSON document whose same key changed differently in the merge bases conflicts again when a tip changes it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), j JSON);",
                "INSERT INTO t VALUES (1, 'seed', '{}');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x', j = '{\"k\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y', j = '{\"k\": 2}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', j = '{\"k\": 3}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', j = '{\"k\": 3}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET j = '{\"k\": 4}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_a, CAST(base_j AS text), CAST(our_j AS text), CAST(their_j AS text) FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("base_j", TEXT), Column("our_j", TEXT), Column("their_j", TEXT)],
                        rows: &[
                            &[T("seed"), T("{}"), T("{\"k\": 4}"), T("{\"k\": 3}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless merge bases that conflict on a row, resolved differently: merge y into x",
            intent: "A keyless row whose copy count conflicted between the merge bases conflicts again when the tips keep different counts.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES ('r', '1');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "INSERT INTO t VALUES ('r', '1'), ('n', 'x');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "DELETE FROM t WHERE a = 'r';",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "DELETE FROM dolt_conflicts_t;",
                "DELETE FROM t WHERE a = 'r';",
                "INSERT INTO t VALUES ('r', '1');",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "DELETE FROM dolt_conflicts_t;",
                "DELETE FROM t WHERE a = 'r';",
                "INSERT INTO t VALUES ('r', '1');",
                "INSERT INTO t VALUES ('r', '1');",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_a, dolt_base_cardinality, dolt_our_cardinality, dolt_their_cardinality FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("dolt_base_cardinality", INT8), Column("dolt_our_cardinality", INT8), Column("dolt_their_cardinality", INT8)],
                        rows: &[
                            &[T("r"), T("1"), T("2"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases with a schema conflict, tips agreeing on the schema and not on a value: merge y into x",
            intent: "With a schema conflict between the merge bases, tips that add the same column with different values conflict on the value.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c int;",
                "INSERT INTO t VALUES (2, 'two', NULL);",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c int;",
                "SELECT dolt_commit('-Am', 'ym');",
                "SELECT dolt_branch('ym');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'xm');",
                "SELECT dolt_branch('xm');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'ylater');",
                "SELECT dolt_branch('ylater');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c int;",
                "UPDATE t SET c = 5 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
            name: "merge bases with a schema conflict, tips disagreeing on the schema: merge y into x",
            intent: "With a schema conflict between the merge bases, tips that disagree on the column have the schema conflict one merge base would give.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c int;",
                "INSERT INTO t VALUES (2, 'two', NULL);",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c int;",
                "SELECT dolt_commit('-Am', 'ym');",
                "SELECT dolt_branch('ym');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'xm');",
                "SELECT dolt_branch('xm');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'ylater');",
                "SELECT dolt_branch('ylater');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "UPDATE t SET c = 5 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                            &[T("public.t"), T("incompatible column types for column 'c': character varying(100) and integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\", and the description names Postgres' column types where Dolt's names its MySQL ones")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless merge bases with different schemas, then a delete of a row they merged cleanly: merge y into x",
            intent: "Keyless merge bases with different schemas match rows by value under the base's schema, so the tips' rows conflict under their new schema.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES ('r', '1');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "INSERT INTO t VALUES ('r', '1', NULL), ('n', 'x', NULL);",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "DELETE FROM t WHERE a = 'r';",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "DELETE FROM dolt_conflicts_t;",
                "DELETE FROM t WHERE a = 'r';",
                "INSERT INTO t VALUES ('r', '1', NULL);",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "DELETE FROM dolt_conflicts_t;",
                "DELETE FROM t WHERE a = 'r';",
                "INSERT INTO t VALUES ('r', '1', NULL);",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "DELETE FROM t WHERE a = 'n';",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", VARCHAR), Column("b", VARCHAR), Column("c", VARCHAR)],
                        rows: &[
                            &[T("r"), T("1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_a, dolt_base_cardinality, dolt_our_cardinality, dolt_their_cardinality FROM dolt_conflicts_t ORDER BY base_a;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("dolt_base_cardinality", INT8), Column("dolt_our_cardinality", INT8), Column("dolt_their_cardinality", INT8)],
                        rows: &[
                            &[T("n"), T("1"), T("0"), T("1")],
                            &[T("r"), T("1"), T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            skip: Some("a merge matches the rows of a table without a primary key across a column both sides added, where Dolt conflicts on every row, in merges with one merge base too"),
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases with chained stored generated columns, resolved differently: merge y into x",
            intent: "A generated column of a generated column is computed in order in the virtual merge base.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a int, b int, g int GENERATED ALWAYS AS (a + b) STORED, h int GENERATED ALWAYS AS (g * 10) STORED);",
                "INSERT INTO t (id, a, b) VALUES (1, 0, 0);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 1, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 3 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 20, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 10, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_a, base_b, base_g, base_h FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", INT4), Column("base_b", INT4), Column("base_g", INT4), Column("base_h", INT4)],
                        rows: &[
                            &[T("0"), T("1"), T("1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: a Postgres generated column can't read another generated column"),
            ..S
        },
        ScriptTest {
            name: "merge bases that conflict on a row and merge its JSON document, with @@dolt_dont_merge_json: merge y into x",
            intent: "The virtual merge base merges the JSON documents whatever dolt_dont_merge_json is, so the tip's later change is the only one.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), j JSON);",
                "INSERT INTO t VALUES (1, 'seed', '{}');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x', j = '{\"x\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y', j = '{\"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', j = '{\"x\": 1, \"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', j = '{\"x\": 1, \"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET j = '{\"x\": 2, \"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
                "SET dolt_dont_merge_json = 1;",
            ],
            assertions: &[
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("j", JSON)],
                        rows: &[
                            &[T("1"), T("resolved"), T("{\"x\": 2, \"y\": 1}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases, one side resolved to the old value, then a drop of the conflicted column: merge y into x",
            intent: "Dropping a column one tip resolved back to the old value still conflicts, as that value is a change from the conflicted merge bases.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t DROP COLUMN a;",
                "SELECT dolt_commit('-am', 'y3');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
            ],
            ..S
        },
        ScriptTest {
            name: "nested criss-cross, a row added twice, then kept and deleted: merge y into x",
            intent: "In a nested criss-cross, a row added differently stays marked through both virtual merge bases, so keeping and deleting it conflicts.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'q') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y3');",
                "SELECT dolt_branch('y3');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x3');",
                "SELECT dolt_branch('x3');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
            ],
            ..S
        },
        ScriptTest {
            name: "nested criss-cross, a conflicted row, then deleted and kept with the old value: merge y into x",
            intent: "In a nested criss-cross, a row conflicted in the first virtual merge base stays marked in the second, so deleting and keeping it conflicts.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (1, 'q') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y3');",
                "SELECT dolt_branch('y3');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x3');",
                "SELECT dolt_branch('x3');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that give a row a time default: merge y into x",
            intent: "A time default in the virtual merge base takes the later merge base's committer date in UTC, whatever the session's time zone, which the merge restores.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN ts timestamp(6) DEFAULT CURRENT_TIMESTAMP(6);",
                "UPDATE t SET a = 'x' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y2';",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x2';",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET TIME ZONE 'Etc/GMT+7';",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's time_zone is Postgres' TimeZone, and Etc/GMT+7 is UTC-07:00")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_ts = (SELECT MAX(date) FROM dolt_log('--all') WHERE message IN ('x1', 'y1')) AT TIME ZONE 'UTC' FROM dolt_preview_merge_conflicts('x', 'y', 't') WHERE base_id = 2;",
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
                    query: "SET TIME ZONE 'UTC';",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's time_zone is Postgres' TimeZone, and Etc/GMT+7 is UTC-07:00")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_ts = (SELECT MAX(date) FROM dolt_log('--all') WHERE message IN ('x1', 'y1')) AT TIME ZONE 'UTC' FROM dolt_preview_merge_conflicts('x', 'y', 't') WHERE base_id = 2;",
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
                    query: "SET TIME ZONE 'Etc/GMT+7';",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's time_zone is Postgres' TimeZone, and Etc/GMT+7 is UTC-07:00")),
                    ..A
                },
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
                    query: "SELECT current_setting('TimeZone');",
                    expected: Expected::Rows {
                        columns: &[Column("current_setting", TEXT)],
                        rows: &[
                            &[T("Etc/GMT+7")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's time_zone is Postgres' TimeZone, and Etc/GMT+7 is UTC-07:00")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_ts = (SELECT MAX(date) FROM dolt_log('--all') WHERE message IN ('x1', 'y1')) AT TIME ZONE 'UTC' FROM dolt_conflicts_t WHERE base_id = 2;",
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
fn test_merge_multiple_bases_resolve_ours_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "long-running schema branch with a conflict: resolve with ours",
            intent: "A row edited on both long-running branches conflicts against its value at their last merge, while another row merges column by column. Resolving the conflict with --ours and committing gives git's result for the same history.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100), w VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c0');",
                "SELECT dolt_branch('schema');",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed'), (3, 'seed', 'seed');",
                "SELECT dolt_commit('-am', 'c1');",
                "SELECT dolt_branch('feature');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x1 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (101, 'main');",
                "UPDATE t SET v = 'main 1' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm1');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (201, 'feature');",
                "SELECT dolt_commit('-am', 'feat1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f1');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x2 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (102, 'main');",
                "UPDATE t SET v = 'main 2' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm2');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (202, 'feature');",
                "SELECT dolt_commit('-am', 'feat2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f2');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x3 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's3');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (103, 'main');",
                "UPDATE t SET v = 'main 3' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (203, 'feature');",
                "SELECT dolt_commit('-am', 'feat3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f3');",
                "SELECT * FROM dolt_merge('main', '-m', 'sync');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x4 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's4');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (104, 'main');",
                "UPDATE t SET v = 'main 4' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm4');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (204, 'feature');",
                "SELECT dolt_commit('-am', 'feat4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f4');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x5 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's5');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (105, 'main');",
                "UPDATE t SET v = 'main 5' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "UPDATE t SET v = 'main' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'main5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm5');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (205, 'feature');",
                "UPDATE t SET v = 'feature 5' WHERE id = 1;",
                "UPDATE t SET w = 'feature' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'feat5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f5');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x6 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's6');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (106, 'main');",
                "UPDATE t SET v = 'main 6' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm6');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (206, 'feature');",
                "SELECT dolt_commit('-am', 'feat6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f6');",
                "SELECT * FROM dolt_checkout('feature');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge main');",
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
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolve merge of main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v, w FROM t WHERE id IN (1, 3) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("w", VARCHAR)],
                        rows: &[
                            &[T("1"), T("feature 5"), T("seed")],
                            &[T("3"), T("main"), T("feature")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
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
        ScriptTest {
            name: "conflicting merge bases, resolved differently: resolve with ours",
            intent: "Merge bases that conflicted, resolved differently on the tips, conflict again with the merge bases' own base as the base value. Resolving the conflict with --ours and committing gives git's result for the same history.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolve merge of y');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("x wins"), T("seed")],
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
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
        ScriptTest {
            name: "conflicting merge bases, one side resolved to the old value: resolve with ours",
            intent: "A tip resolved back to the merge bases' base value still changed it from the conflicted merge bases, as in git, so it conflicts with the other tip. Resolving the conflict with --ours and committing gives git's result for the same history.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolve merge of y');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), T("seed")],
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
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
        ScriptTest {
            name: "merge bases that conflict on a deleted row, resolved differently: resolve with ours",
            intent: "A row one tip deleted and the other kept, after the merge bases conflicted on it, conflicts, as in git. Resolving the conflict with --ours and committing gives git's result for the same history.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "DELETE FROM t WHERE id = 1;",
                "UPDATE t SET a = 'x' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (1, 'y', 'seed') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT dolt_conflicts_resolve('--ours', 't');",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolve merge of y');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
fn test_merge_multiple_bases_resolve_theirs_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "long-running schema branch with a conflict: resolve with theirs",
            intent: "A row edited on both long-running branches conflicts against its value at their last merge, while another row merges column by column. Resolving the conflict with --theirs and committing gives git's result for the same history.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100), w VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c0');",
                "SELECT dolt_branch('schema');",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed'), (3, 'seed', 'seed');",
                "SELECT dolt_commit('-am', 'c1');",
                "SELECT dolt_branch('feature');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x1 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (101, 'main');",
                "UPDATE t SET v = 'main 1' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm1');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (201, 'feature');",
                "SELECT dolt_commit('-am', 'feat1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f1');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x2 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (102, 'main');",
                "UPDATE t SET v = 'main 2' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm2');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (202, 'feature');",
                "SELECT dolt_commit('-am', 'feat2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f2');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x3 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's3');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (103, 'main');",
                "UPDATE t SET v = 'main 3' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (203, 'feature');",
                "SELECT dolt_commit('-am', 'feat3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f3');",
                "SELECT * FROM dolt_merge('main', '-m', 'sync');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x4 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's4');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (104, 'main');",
                "UPDATE t SET v = 'main 4' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm4');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (204, 'feature');",
                "SELECT dolt_commit('-am', 'feat4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f4');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x5 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's5');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (105, 'main');",
                "UPDATE t SET v = 'main 5' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "UPDATE t SET v = 'main' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'main5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm5');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (205, 'feature');",
                "UPDATE t SET v = 'feature 5' WHERE id = 1;",
                "UPDATE t SET w = 'feature' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'feat5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f5');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x6 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's6');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (106, 'main');",
                "UPDATE t SET v = 'main 6' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm6');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (206, 'feature');",
                "SELECT dolt_commit('-am', 'feat6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f6');",
                "SELECT * FROM dolt_checkout('feature');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge main');",
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
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolve merge of main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v, w FROM t WHERE id IN (1, 3) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("w", VARCHAR)],
                        rows: &[
                            &[T("1"), T("main 6"), T("seed")],
                            &[T("3"), T("main"), T("feature")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
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
        ScriptTest {
            name: "conflicting merge bases, resolved differently: resolve with theirs",
            intent: "Merge bases that conflicted, resolved differently on the tips, conflict again with the merge bases' own base as the base value. Resolving the conflict with --theirs and committing gives git's result for the same history.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolve merge of y');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("y wins"), T("seed")],
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
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
        ScriptTest {
            name: "conflicting merge bases, one side resolved to the old value: resolve with theirs",
            intent: "A tip resolved back to the merge bases' base value still changed it from the conflicted merge bases, as in git, so it conflicts with the other tip. Resolving the conflict with --theirs and committing gives git's result for the same history.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolve merge of y');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("y"), T("seed")],
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
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
        ScriptTest {
            name: "merge bases that conflict on a deleted row, resolved differently: resolve with theirs",
            intent: "A row one tip deleted and the other kept, after the merge bases conflicted on it, conflicts, as in git. Resolving the conflict with --theirs and committing gives git's result for the same history.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "DELETE FROM t WHERE id = 1;",
                "UPDATE t SET a = 'x' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (1, 'y', 'seed') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT dolt_conflicts_resolve('--theirs', 't');",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolve merge of y');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("y"), T("seed")],
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
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
fn test_merge_multiple_bases_resolve_by_edit_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "long-running schema branch with a conflict: resolve by editing the row",
            intent: "A row edited on both long-running branches conflicts against its value at their last merge, while another row merges column by column. Resolving the conflict by editing the row and committing gives git's result for the same history.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100), w VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c0');",
                "SELECT dolt_branch('schema');",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed'), (3, 'seed', 'seed');",
                "SELECT dolt_commit('-am', 'c1');",
                "SELECT dolt_branch('feature');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x1 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (101, 'main');",
                "UPDATE t SET v = 'main 1' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm1');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (201, 'feature');",
                "SELECT dolt_commit('-am', 'feat1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f1');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x2 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (102, 'main');",
                "UPDATE t SET v = 'main 2' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm2');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (202, 'feature');",
                "SELECT dolt_commit('-am', 'feat2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f2');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x3 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's3');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (103, 'main');",
                "UPDATE t SET v = 'main 3' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (203, 'feature');",
                "SELECT dolt_commit('-am', 'feat3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f3');",
                "SELECT * FROM dolt_merge('main', '-m', 'sync');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x4 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's4');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (104, 'main');",
                "UPDATE t SET v = 'main 4' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm4');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (204, 'feature');",
                "SELECT dolt_commit('-am', 'feat4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f4');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x5 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's5');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (105, 'main');",
                "UPDATE t SET v = 'main 5' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "UPDATE t SET v = 'main' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'main5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm5');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (205, 'feature');",
                "UPDATE t SET v = 'feature 5' WHERE id = 1;",
                "UPDATE t SET w = 'feature' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'feat5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f5');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x6 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's6');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (106, 'main');",
                "UPDATE t SET v = 'main 6' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm6');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (206, 'feature');",
                "SELECT dolt_commit('-am', 'feat6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f6');",
                "SELECT * FROM dolt_checkout('feature');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge main');",
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
                    query: "INSERT INTO t (id, v, w, x1, x2, x3, x4, x5, x6) VALUES (1, 'edited', 'edited', 'edited', 'edited', 'edited', 'edited', 'edited', 'edited') ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v, w = EXCLUDED.w, x1 = EXCLUDED.x1, x2 = EXCLUDED.x2, x3 = EXCLUDED.x3, x4 = EXCLUDED.x4, x5 = EXCLUDED.x5, x6 = EXCLUDED.x6;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t;",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolve merge of main');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v, w, x6 FROM t WHERE id IN (1, 3) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("w", VARCHAR), Column("x6", VARCHAR)],
                        rows: &[
                            &[T("1"), T("edited"), T("edited"), T("edited")],
                            &[T("3"), T("main"), T("feature"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
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
        ScriptTest {
            name: "conflicting merge bases, resolved differently: resolve by editing the row",
            intent: "Merge bases that conflicted, resolved differently on the tips, conflict again with the merge bases' own base as the base value. Resolving the conflict by editing the row and committing gives git's result for the same history.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "INSERT INTO t VALUES (1, 'edited', 'edited') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t;",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolve merge of y');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("edited"), T("edited")],
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
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
        ScriptTest {
            name: "conflicting merge bases, one side resolved to the old value: resolve by editing the row",
            intent: "A tip resolved back to the merge bases' base value still changed it from the conflicted merge bases, as in git, so it conflicts with the other tip. Resolving the conflict by editing the row and committing gives git's result for the same history.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "INSERT INTO t VALUES (1, 'edited', 'edited') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t;",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolve merge of y');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("edited"), T("edited")],
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
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
        ScriptTest {
            name: "merge bases that conflict on a deleted row, resolved differently: resolve by editing the row",
            intent: "A row one tip deleted and the other kept, after the merge bases conflicted on it, conflicts, as in git. Resolving the conflict by editing the row and committing gives git's result for the same history.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "DELETE FROM t WHERE id = 1;",
                "UPDATE t SET a = 'x' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (1, 'y', 'seed') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "INSERT INTO t VALUES (1, 'edited', 'edited') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t;",
                    expected: Expected::Ok,
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'resolve merge of y');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("edited"), T("edited")],
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
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
fn test_merge_multiple_bases_abort_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "long-running schema branch with a conflict: abort",
            intent: "A row edited on both long-running branches conflicts against its value at their last merge, while another row merges column by column. Aborting the merge restores the working set from before it.",
            set_up_script: &[
                "CREATE TABLE t (id int PRIMARY KEY, v VARCHAR(100), w VARCHAR(100));",
                "SELECT dolt_commit('-Am', 'c0');",
                "SELECT dolt_branch('schema');",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed'), (3, 'seed', 'seed');",
                "SELECT dolt_commit('-am', 'c1');",
                "SELECT dolt_branch('feature');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x1 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (101, 'main');",
                "UPDATE t SET v = 'main 1' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm1');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (201, 'feature');",
                "SELECT dolt_commit('-am', 'feat1');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f1');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x2 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's2');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (102, 'main');",
                "UPDATE t SET v = 'main 2' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm2');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (202, 'feature');",
                "SELECT dolt_commit('-am', 'feat2');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f2');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x3 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's3');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (103, 'main');",
                "UPDATE t SET v = 'main 3' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm3');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (203, 'feature');",
                "SELECT dolt_commit('-am', 'feat3');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f3');",
                "SELECT * FROM dolt_merge('main', '-m', 'sync');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x4 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's4');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (104, 'main');",
                "UPDATE t SET v = 'main 4' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm4');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (204, 'feature');",
                "SELECT dolt_commit('-am', 'feat4');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f4');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x5 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's5');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (105, 'main');",
                "UPDATE t SET v = 'main 5' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "UPDATE t SET v = 'main' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'main5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm5');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (205, 'feature');",
                "UPDATE t SET v = 'feature 5' WHERE id = 1;",
                "UPDATE t SET w = 'feature' WHERE id = 3;",
                "SELECT dolt_commit('-am', 'feat5');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f5');",
                "SELECT * FROM dolt_checkout('schema');",
                "ALTER TABLE t ADD COLUMN x6 VARCHAR(100);",
                "SELECT dolt_commit('-am', 's6');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t (id, v) VALUES (106, 'main');",
                "UPDATE t SET v = 'main 6' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'main6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'm6');",
                "SELECT * FROM dolt_checkout('feature');",
                "INSERT INTO t (id, v) VALUES (206, 'feature');",
                "SELECT dolt_commit('-am', 'feat6');",
                "SELECT * FROM dolt_merge('schema', '-m', 'f6');",
                "SELECT * FROM dolt_checkout('feature');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main', '-m', 'merge main');",
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
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge aborted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v, w FROM t WHERE id IN (1, 2, 3) OR id > 103 AND id < 200 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", VARCHAR), Column("w", VARCHAR)],
                        rows: &[
                            &[T("1"), T("feature 5"), T("seed")],
                            &[T("2"), T("seed"), T("seed")],
                            &[T("3"), T("seed"), T("feature")],
                        ],
                        tag: "SELECT 3",
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
        ScriptTest {
            name: "conflicting merge bases, resolved differently: abort",
            intent: "Merge bases that conflicted, resolved differently on the tips, conflict again with the merge bases' own base as the base value. Aborting the merge restores the working set from before it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge aborted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("x wins"), T("seed")],
                            &[T("2"), T("x"), T("y")],
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
        ScriptTest {
            name: "conflicting merge bases, one side resolved to the old value: abort",
            intent: "A tip resolved back to the merge bases' base value still changed it from the conflicted merge bases, as in git, so it conflicts with the other tip. Aborting the merge restores the working set from before it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge aborted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), T("seed")],
                            &[T("2"), T("x"), T("y")],
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
        ScriptTest {
            name: "merge bases that conflict on a deleted row, resolved differently: abort",
            intent: "A row one tip deleted and the other kept, after the merge bases conflicted on it, conflicts, as in git. Aborting the merge restores the working set from before it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "DELETE FROM t WHERE id = 1;",
                "UPDATE t SET a = 'x' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (1, 'y', 'seed') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge aborted")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("2"), T("x"), T("y")],
                        ],
                        tag: "SELECT 1",
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
fn test_merge_multiple_bases_preview_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "preview: conflicting merge bases, one side resolved to the old value: merge y into x",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of conflicting merge bases, one side resolved to the old value.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('x', 'y');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_a, our_a, their_a FROM dolt_preview_merge_conflicts('x', 'y', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("seed"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y into x');",
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
                    query: "SELECT base_a, our_a, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("seed"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: conflicting merge bases, one side resolved to the old value: merge x into y",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of conflicting merge bases, one side resolved to the old value.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('y', 'x');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_a, our_a, their_a FROM dolt_preview_merge_conflicts('y', 'x', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("y"), T("seed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x into y');",
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
                    query: "SELECT base_a, our_a, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("y"), T("seed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: conflicting merge bases with a virtual generated column first: merge y into x",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of conflicting merge bases with a virtual generated column first.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, g int GENERATED ALWAYS AS (id * 2) VIRTUAL, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('x', 'y');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_a, base_b, our_a, their_a FROM dolt_preview_merge_conflicts('x', 'y', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("base_b", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("seed"), T("x wins"), T("y wins")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y into x');",
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
                    query: "SELECT base_a, base_b, our_a, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("base_b", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("seed"), T("x wins"), T("y wins")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: conflicting merge bases with a stored generated column, resolved differently: merge y into x",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of conflicting merge bases with a stored generated column, resolved differently.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a int, b int, g int GENERATED ALWAYS AS (a + b) STORED); CREATE INDEX ib ON t (b); CREATE INDEX ig ON t (g);",
                "INSERT INTO t (id, a, b) VALUES (1, 0, 0);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 1, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 3 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 20, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 10, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('x', 'y');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_a, base_b, base_g, our_a, their_a FROM dolt_preview_merge_conflicts('x', 'y', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", INT4), Column("base_b", INT4), Column("base_g", INT4), Column("our_a", INT4), Column("their_a", INT4)],
                        rows: &[
                            &[T("0"), T("1"), T("1"), T("10"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y into x');",
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
                    query: "SELECT base_a, base_b, base_g, our_a, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", INT4), Column("base_b", INT4), Column("base_g", INT4), Column("our_a", INT4), Column("their_a", INT4)],
                        rows: &[
                            &[T("0"), T("1"), T("1"), T("10"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: merge bases that conflict on a row and merge its JSON document: merge y into x",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of merge bases that conflict on a row and merge its JSON document.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), j JSON);",
                "INSERT INTO t VALUES (1, 'seed', '{}');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x', j = '{\"x\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y', j = '{\"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', j = '{\"x\": 1, \"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', j = '{\"x\": 1, \"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET j = '{\"x\": 2, \"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('x', 'y');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y into x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: conflicting merge bases, resolved the same way: merge y into x",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of conflicting merge bases, resolved the same way.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('x', 'y');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y into x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: conflicting merge bases, resolved the same way, then a type change: merge y into x",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of conflicting merge bases, resolved the same way, then a type change.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t ALTER COLUMN a TYPE TEXT;",
                "SELECT dolt_commit('-am', 'y3');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('x', 'y');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y into x');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: merge bases that add a row with different values, one side deleting it: merge y into x",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of merge bases that add a row with different values, one side deleting it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('x', 'y');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_id, our_id, their_id, their_a FROM dolt_preview_merge_conflicts('x', 'y', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("our_id", INT4), Column("their_id", INT4), Column("their_a", VARCHAR)],
                        rows: &[
                            &[Null, Null, T("3"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y into x');",
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
                    query: "SELECT base_id, our_id, their_id, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("our_id", INT4), Column("their_id", INT4), Column("their_a", VARCHAR)],
                        rows: &[
                            &[Null, Null, T("3"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: merge bases that add a row with different values, one side deleting it: merge x into y",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of merge bases that add a row with different values, one side deleting it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('y', 'x');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_id, our_id, our_a, their_id FROM dolt_preview_merge_conflicts('y', 'x', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("our_id", INT4), Column("our_a", VARCHAR), Column("their_id", INT4)],
                        rows: &[
                            &[Null, T("3"), T("y"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x into y');",
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
                    query: "SELECT base_id, our_id, our_a, their_id FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("our_id", INT4), Column("our_a", VARCHAR), Column("their_id", INT4)],
                        rows: &[
                            &[Null, T("3"), T("y"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: conflicting merge bases with different schemas: merge y into x",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of conflicting merge bases with different schemas.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "UPDATE t SET a = 'x' WHERE id = 1;",
                "INSERT INTO t VALUES (3, 'x', 'x', 'x');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "DELETE FROM t WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('x', 'y');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_id, base_a, our_a, their_a FROM dolt_preview_merge_conflicts('x', 'y', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), T("x wins"), T("seed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y into x');",
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
                    query: "SELECT base_id, base_a, our_a, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), T("x wins"), T("seed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: nested criss-cross: merge y into x",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of nested criss-cross.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'q' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y3');",
                "SELECT dolt_branch('y3');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x3');",
                "SELECT dolt_branch('x3');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('x', 'y');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_a, our_a, their_a FROM dolt_preview_merge_conflicts('x', 'y', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("y"), T("q")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y into x');",
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
                    query: "SELECT base_a, our_a, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("y"), T("q")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: nested criss-cross: merge x into y",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of nested criss-cross.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'q' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y3');",
                "SELECT dolt_branch('y3');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x3');",
                "SELECT dolt_branch('x3');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('y', 'x');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_a, our_a, their_a FROM dolt_preview_merge_conflicts('y', 'x', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("q"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x into y');",
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
                    query: "SELECT base_a, our_a, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("q"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: three merge bases that conflict in different folds: merge m2 into m1",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of three merge bases that conflict in different folds.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed'), (3, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('p1');",
                "SELECT dolt_branch('p2');",
                "SELECT dolt_branch('p3');",
                "SELECT dolt_branch('m1');",
                "SELECT dolt_branch('m2');",
                "SELECT * FROM dolt_checkout('p1');",
                "UPDATE t SET a = 'p1' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'p1');",
                "SELECT * FROM dolt_checkout('p2');",
                "UPDATE t SET a = 'p2', b = 'p2' WHERE id = 1;",
                "UPDATE t SET b = 'p2' WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'p2');",
                "SELECT * FROM dolt_checkout('p3');",
                "UPDATE t SET b = 'p3' WHERE id = 1;",
                "UPDATE t SET a = 'p3' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'p3');",
                "SELECT * FROM dolt_checkout('m1');",
                "INSERT INTO t VALUES (10, 'm1', NULL);",
                "SELECT dolt_commit('-Am', 'm1start');",
                "SELECT dolt_branch('m1start');",
                "SELECT * FROM dolt_checkout('m1');",
                "SELECT * FROM dolt_merge('p1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'm1 merges p1');",
                "SELECT * FROM dolt_checkout('m1');",
                "SELECT * FROM dolt_merge('p2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "SELECT dolt_commit('-Am', 'm1 merges p2');",
                "SELECT * FROM dolt_checkout('m1');",
                "SELECT * FROM dolt_merge('p3', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "SELECT dolt_commit('-Am', 'm1 merges p3');",
                "SELECT * FROM dolt_checkout('m1');",
                "INSERT INTO t VALUES (1, 'm1', 'both') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'm1later');",
                "SELECT dolt_branch('m1later');",
                "SELECT * FROM dolt_checkout('m2');",
                "INSERT INTO t VALUES (20, 'm2', NULL);",
                "SELECT dolt_commit('-Am', 'm2start');",
                "SELECT dolt_branch('m2start');",
                "SELECT * FROM dolt_checkout('m2');",
                "SELECT * FROM dolt_merge('p3', '--no-commit');",
                "SELECT dolt_commit('-Am', 'm2 merges p3');",
                "SELECT * FROM dolt_checkout('m2');",
                "SELECT * FROM dolt_merge('p2', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "SELECT dolt_commit('-Am', 'm2 merges p2');",
                "SELECT * FROM dolt_checkout('m2');",
                "SELECT * FROM dolt_merge('p1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "SELECT dolt_commit('-Am', 'm2 merges p1');",
                "SELECT * FROM dolt_checkout('m2');",
                "INSERT INTO t VALUES (1, 'seed', 'both') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "INSERT INTO t VALUES (2, 'seed', 'seed') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a, b = EXCLUDED.b;",
                "SELECT dolt_commit('-Am', 'm2later');",
                "SELECT dolt_branch('m2later');",
                "SELECT * FROM dolt_checkout('m1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('m1', 'm2');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_id, base_a, our_a, their_a FROM dolt_preview_merge_conflicts('m1', 'm2', 't');",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), T("m1"), T("seed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('m2', '-m', 'merge m2 into m1');",
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
                    query: "SELECT base_id, base_a, our_a, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("our_a", VARCHAR), Column("their_a", VARCHAR)],
                        rows: &[
                            &[T("1"), T("seed"), T("m1"), T("seed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "preview: conflicting merge bases, one side dropping the table: merge y into x",
            intent: "dolt_preview_merge_conflicts and its summary report, without merging, the conflicts of conflicting merge bases, one side dropping the table.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t (id, a, b) VALUES (1, 'seed', 'seed'), (2, 'seed', 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x';",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y' WHERE id = 1;",
                "UPDATE t SET b = 'y' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'seed' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "DROP TABLE t;",
                "SELECT dolt_commit('-am', 'x3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', (dolt_hashof('HEAD'))::text, FALSE);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('x', 'y');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), Null, T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
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
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y into x');",
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
            ],
            ..S
        },
    ]);
}

#[test]
fn test_merge_multiple_bases_both_directions_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "merge bases that conflict on two cells of a row, then an edit to a clean cell: merge y into x",
            intent: "Cells that merged cleanly between the merge bases take a later edit without conflict, though other cells of the row conflicted.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100), c VARCHAR(100), d VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed', 'seed', NULL);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x', b = 'x', d = 'x' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y', c = 'y', d = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', b = 'x', c = 'y', d = NULL WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', b = 'x', c = 'y', d = NULL WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET b = 'x later' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR), Column("c", VARCHAR), Column("d", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved"), T("x later"), T("y"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that conflict on two cells of a row, then an edit to a clean cell: merge x into y",
            intent: "Cells that merged cleanly between the merge bases take a later edit without conflict, though other cells of the row conflicted.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100), c VARCHAR(100), d VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed', 'seed', NULL);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x', b = 'x', d = 'x' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y', c = 'y', d = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', b = 'x', c = 'y', d = NULL WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', b = 'x', c = 'y', d = NULL WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET b = 'x later' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("b", VARCHAR), Column("c", VARCHAR), Column("d", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved"), T("x later"), T("y"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that conflict on two cells of a row, resolved differently in the second: merge y into x",
            intent: "A tip that keeps NULL in a conflicted cell whose base is NULL still changed it from the merge bases, so it conflicts.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100), c VARCHAR(100), d VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed', 'seed', NULL);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x', b = 'x', d = 'x' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y', c = 'y', d = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', b = 'x', c = 'y', d = NULL WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', b = 'x', c = 'y', d = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_a, base_b, base_c, base_d, our_a, our_b, our_c, our_d, their_a, their_b, their_c, their_d FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("base_b", VARCHAR), Column("base_c", VARCHAR), Column("base_d", VARCHAR), Column("our_a", VARCHAR), Column("our_b", VARCHAR), Column("our_c", VARCHAR), Column("our_d", VARCHAR), Column("their_a", VARCHAR), Column("their_b", VARCHAR), Column("their_c", VARCHAR), Column("their_d", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("x"), T("y"), Null, T("resolved"), T("x"), T("y"), T("x wins"), T("resolved"), T("x"), T("y"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that conflict on two cells of a row, resolved differently in the second: merge x into y",
            intent: "A tip that keeps NULL in a conflicted cell whose base is NULL still changed it from the merge bases, so it conflicts.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), b VARCHAR(100), c VARCHAR(100), d VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed', 'seed', 'seed', NULL);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x', b = 'x', d = 'x' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y', c = 'y', d = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', b = 'x', c = 'y', d = NULL WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', b = 'x', c = 'y', d = 'x wins' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
            ],
            assertions: &[
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
                    query: "SELECT base_a, base_b, base_c, base_d, their_a, their_b, their_c, their_d, our_a, our_b, our_c, our_d FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_a", VARCHAR), Column("base_b", VARCHAR), Column("base_c", VARCHAR), Column("base_d", VARCHAR), Column("their_a", VARCHAR), Column("their_b", VARCHAR), Column("their_c", VARCHAR), Column("their_d", VARCHAR), Column("our_a", VARCHAR), Column("our_b", VARCHAR), Column("our_c", VARCHAR), Column("our_d", VARCHAR)],
                        rows: &[
                            &[T("seed"), T("x"), T("y"), Null, T("resolved"), T("x"), T("y"), T("x wins"), T("resolved"), T("x"), T("y"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that add a row with different values, resolved differently: merge y into x",
            intent: "A row the merge bases added differently has no base row, so its conflict has a NULL base key.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_id, base_a, our_id, our_a, their_id, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("our_id", INT4), Column("our_a", VARCHAR), Column("their_id", INT4), Column("their_a", VARCHAR)],
                        rows: &[
                            &[Null, Null, T("3"), T("x"), T("3"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that add a row with different values, resolved differently: merge x into y",
            intent: "A row the merge bases added differently has no base row, so its conflict has a NULL base key.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
            ],
            assertions: &[
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
                    query: "SELECT base_id, base_a, their_id, their_a, our_id, our_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("their_id", INT4), Column("their_a", VARCHAR), Column("our_id", INT4), Column("our_a", VARCHAR)],
                        rows: &[
                            &[Null, Null, T("3"), T("x"), T("3"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that add a row with different values, one side deleting it: merge y into x",
            intent: "A row the merge bases added differently conflicts when one tip deletes it and the other keeps it, as in git.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_id, base_a, our_id, our_a, their_id, their_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("our_id", INT4), Column("our_a", VARCHAR), Column("their_id", INT4), Column("their_a", VARCHAR)],
                        rows: &[
                            &[Null, Null, Null, Null, T("3"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that add a row with different values, one side deleting it: merge x into y",
            intent: "A row the merge bases added differently conflicts when one tip deletes it and the other keeps it, as in git.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "INSERT INTO t VALUES (3, 'x') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "INSERT INTO t VALUES (3, 'y') ON CONFLICT (id) DO UPDATE SET a = EXCLUDED.a;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "DELETE FROM t WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
            ],
            assertions: &[
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
                    query: "SELECT base_id, base_a, their_id, their_a, our_id, our_a FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_a", VARCHAR), Column("their_id", INT4), Column("their_a", VARCHAR), Column("our_id", INT4), Column("our_a", VARCHAR)],
                        rows: &[
                            &[Null, Null, Null, Null, T("3"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting merge bases with a stored generated column, then a change to another source: merge y into x",
            intent: "A stored generated value is computed, not merged, so only its conflicting source conflicts and a later change to another source merges.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a int, b int, g int GENERATED ALWAYS AS (a + b) STORED); CREATE INDEX ib ON t (b); CREATE INDEX ig ON t (g);",
                "INSERT INTO t (id, a, b) VALUES (1, 0, 0);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 1, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 3 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 10, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 10, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET b = 2 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("2"), T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t  WHERE g = 12;",
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
        ScriptTest {
            name: "conflicting merge bases with a stored generated column, then a change to another source: merge x into y",
            intent: "A stored generated value is computed, not merged, so only its conflicting source conflicts and a later change to another source merges.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a int, b int, g int GENERATED ALWAYS AS (a + b) STORED); CREATE INDEX ib ON t (b); CREATE INDEX ig ON t (g);",
                "INSERT INTO t (id, a, b) VALUES (1, 0, 0);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 1, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 3 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 10, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 10, b = 1 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET b = 2 WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("2"), T("12")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t  WHERE g = 12;",
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
        ScriptTest {
            name: "merge bases that conflict on a row and merge its JSON document, then a change to the document: merge y into x",
            intent: "Different keys added to a JSON document merge cleanly in the virtual merge base, so a later change to the document merges.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), j JSON);",
                "INSERT INTO t VALUES (1, 'seed', '{}');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x', j = '{\"x\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y', j = '{\"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', j = '{\"x\": 1, \"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', j = '{\"x\": 1, \"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET j = '{\"x\": 2, \"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("j", JSON)],
                        rows: &[
                            &[T("1"), T("resolved"), T("{\"x\": 2, \"y\": 1}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases that conflict on a row and merge its JSON document, then a change to the document: merge x into y",
            intent: "Different keys added to a JSON document merge cleanly in the virtual merge base, so a later change to the document merges.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100), j JSON);",
                "INSERT INTO t VALUES (1, 'seed', '{}');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET a = 'x', j = '{\"x\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y', j = '{\"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', j = '{\"x\": 1, \"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_conflicts_resolve('--ours', 't');",
                "UPDATE t SET a = 'resolved', j = '{\"x\": 1, \"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET j = '{\"x\": 2, \"y\": 1}' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("j", JSON)],
                        rows: &[
                            &[T("1"), T("resolved"), T("{\"x\": 2, \"y\": 1}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless merge bases that conflict on a row, then a delete of a row they merged cleanly: merge y into x",
            intent: "A keyless row the merge bases merged cleanly can be deleted by a tip without conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES ('r', '1');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "INSERT INTO t VALUES ('r', '1'), ('n', 'x');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "DELETE FROM t WHERE a = 'r';",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "DELETE FROM dolt_conflicts_t;",
                "DELETE FROM t WHERE a = 'r';",
                "INSERT INTO t VALUES ('r', '1');",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "DELETE FROM dolt_conflicts_t;",
                "DELETE FROM t WHERE a = 'r';",
                "INSERT INTO t VALUES ('r', '1');",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "DELETE FROM t WHERE a = 'n';",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("r"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "keyless merge bases that conflict on a row, then a delete of a row they merged cleanly: merge x into y",
            intent: "A keyless row the merge bases merged cleanly can be deleted by a tip without conflict.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (a VARCHAR(100), b VARCHAR(100));",
                "INSERT INTO t VALUES ('r', '1');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "INSERT INTO t VALUES ('r', '1'), ('n', 'x');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "DELETE FROM t WHERE a = 'r';",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "DELETE FROM dolt_conflicts_t;",
                "DELETE FROM t WHERE a = 'r';",
                "INSERT INTO t VALUES ('r', '1');",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "DELETE FROM dolt_conflicts_t;",
                "DELETE FROM t WHERE a = 'r';",
                "INSERT INTO t VALUES ('r', '1');",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('x');",
                "DELETE FROM t WHERE a = 'n';",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
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
                    query: "SELECT * FROM t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", VARCHAR), Column("b", VARCHAR)],
                        rows: &[
                            &[T("r"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases with a schema conflict, tips agreeing on the schema: merge y into x",
            intent: "A schema conflict between the merge bases leaves the virtual merge base the base's schema, and tips agreeing on the new schema merge.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c int;",
                "INSERT INTO t VALUES (2, 'two', NULL);",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c int;",
                "SELECT dolt_commit('-Am', 'ym');",
                "SELECT dolt_branch('ym');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'xm');",
                "SELECT dolt_branch('xm');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'ylater');",
                "SELECT dolt_branch('ylater');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c int;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                            &[T("1"), T("y edit"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases with a schema conflict, tips agreeing on the schema: merge x into y",
            intent: "A schema conflict between the merge bases leaves the virtual merge base the base's schema, and tips agreeing on the new schema merge.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (id int PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c int;",
                "INSERT INTO t VALUES (2, 'two', NULL);",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c int;",
                "SELECT dolt_commit('-Am', 'ym');",
                "SELECT dolt_branch('ym');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'xm');",
                "SELECT dolt_branch('xm');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "DELETE FROM t WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'ylater');",
                "SELECT dolt_branch('ylater');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t DROP COLUMN c;",
                "ALTER TABLE t ADD COLUMN c int;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('x', '-m', 'merge x');",
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
                            &[T("1"), T("y edit"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases with a schema conflict and values that do not convert to the base's schema: merge y into x",
            intent: "Values that don't convert to the base's schema are left out of the virtual merge base's rows rather than failing the merge.",
            set_up_script: &[
                "BEGIN;",
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE t (id int PRIMARY KEY, c int, d VARCHAR(20));",
                "INSERT INTO t VALUES (1, 1, 'seed'), (2, 2, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(20);",
                "UPDATE t SET c = 'not-an-int' WHERE id = 1;",
                "INSERT INTO t VALUES (3, 'not-an-int', 'three');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t ALTER COLUMN c TYPE bigint USING c::bigint;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(20);",
                "SELECT dolt_commit('-Am', 'ym');",
                "SELECT dolt_branch('ym');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET c = '5' WHERE id IN (1, 3);",
                "ALTER TABLE t ALTER COLUMN c TYPE bigint USING c::bigint;",
                "SELECT dolt_commit('-Am', 'xm');",
                "SELECT dolt_branch('xm');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET c = '7' WHERE id IN (1, 3);",
                "ALTER TABLE t ALTER COLUMN c TYPE bigint USING c::bigint;",
                "UPDATE t SET d = 'same' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'ylater');",
                "SELECT dolt_branch('ylater');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET c = 1, d = 'same' WHERE id = 1;",
                "UPDATE t SET c = NULL WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
            ],
            assertions: &[
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
                    query: "SELECT base_id, base_c, base_d, our_c, their_c FROM dolt_conflicts_t ORDER BY base_id;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_c", INT4), Column("base_d", VARCHAR), Column("our_c", INT8), Column("their_c", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("seed"), T("1"), T("7")],
                            &[T("3"), Null, T("three"), Null, T("7")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge bases with a schema conflict and values that do not convert to the base's schema: merge x into y",
            intent: "Values that don't convert to the base's schema are left out of the virtual merge base's rows rather than failing the merge.",
            set_up_script: &[
                "BEGIN;",
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE t (id int PRIMARY KEY, c int, d VARCHAR(20));",
                "INSERT INTO t VALUES (1, 1, 'seed'), (2, 2, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT * FROM dolt_checkout('x');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(20);",
                "UPDATE t SET c = 'not-an-int' WHERE id = 1;",
                "INSERT INTO t VALUES (3, 'not-an-int', 'three');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t ALTER COLUMN c TYPE bigint USING c::bigint;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1');",
                "SELECT * FROM dolt_checkout('y');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(20);",
                "SELECT dolt_commit('-Am', 'ym');",
                "SELECT dolt_branch('ym');",
                "SELECT * FROM dolt_checkout('y');",
                "SELECT * FROM dolt_merge('x1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'y2');",
                "SELECT dolt_branch('y2');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET c = '5' WHERE id IN (1, 3);",
                "ALTER TABLE t ALTER COLUMN c TYPE bigint USING c::bigint;",
                "SELECT dolt_commit('-Am', 'xm');",
                "SELECT dolt_branch('xm');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_merge('y1', '--no-commit');",
                "SELECT dolt_commit('-Am', 'x2');",
                "SELECT dolt_branch('x2');",
                "SELECT * FROM dolt_checkout('y');",
                "UPDATE t SET c = '7' WHERE id IN (1, 3);",
                "ALTER TABLE t ALTER COLUMN c TYPE bigint USING c::bigint;",
                "UPDATE t SET d = 'same' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'ylater');",
                "SELECT dolt_branch('ylater');",
                "SELECT * FROM dolt_checkout('x');",
                "UPDATE t SET c = 1, d = 'same' WHERE id = 1;",
                "UPDATE t SET c = NULL WHERE id = 3;",
                "SELECT dolt_commit('-Am', 'xlater');",
                "SELECT dolt_branch('xlater');",
                "SELECT * FROM dolt_checkout('x');",
                "SELECT * FROM dolt_checkout('y');",
            ],
            assertions: &[
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
                    query: "SELECT base_id, base_c, base_d, their_c, our_c FROM dolt_conflicts_t ORDER BY base_id;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_c", INT4), Column("base_d", VARCHAR), Column("their_c", INT8), Column("our_c", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("seed"), T("1"), T("7")],
                            &[T("3"), Null, T("three"), Null, T("7")],
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
fn test_virtual_merge_base_kept_by_its_conflicts() {
    run_scripts(&[
        ScriptTest {
            name: "a conflict against a virtual merge base reads its base side after a collection",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, v TEXT);",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c0');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT dolt_checkout('x');",
                "INSERT INTO t VALUES (10, 'x');",
                "SELECT dolt_commit('-am', 'x1');",
                "SELECT dolt_branch('x1', 'x');",
                "SELECT dolt_checkout('y');",
                "INSERT INTO t VALUES (20, 'y');",
                "SELECT dolt_commit('-am', 'y1');",
                "SELECT dolt_branch('y1', 'y');",
                "SELECT dolt_merge('x1', '-m', 'my');",
                "UPDATE t SET v = 'y' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'y2');",
                "SELECT dolt_checkout('x');",
                "SELECT dolt_merge('y1', '-m', 'mx');",
                "UPDATE t SET v = 'x' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'x2');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'm');",
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
                    query: "SELECT base_id, base_v, our_v, their_v FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_v", TEXT), Column("our_v", TEXT), Column("their_v", TEXT)],
                        rows: &[
                            &[T("1"), T("seed"), T("x"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                ScriptTestAssertion {
                    query: "SELECT base_id, base_v, our_v, their_v FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("base_v", TEXT), Column("our_v", TEXT), Column("their_v", TEXT)],
                        rows: &[
                            &[T("1"), T("seed"), T("x"), T("y")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("10"), T("x")],
                            &[T("20"), T("y")],
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
fn test_virtual_merge_base_reverts_what_depends_on_a_conflicting_definition() {
    run_scripts(&[
        ScriptTest {
            name: "a view on a column the virtual merge base leaves out of a table whose definitions its merge bases conflict on",
            intent: "A view one merge base added on a column of a table whose definitions conflict between the merge bases is left out of the virtual merge base with the column, so the merge keeps it.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN d INT;",
                "INSERT INTO t VALUES (2, 'two', NULL, 4);",
                "CREATE VIEW v AS SELECT id, d FROM t;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1', 'x');",
                "SELECT dolt_checkout('y');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1', 'y');",
                "ALTER TABLE t ALTER COLUMN c TYPE INT USING c::int;",
                "SELECT dolt_commit('-Am', 'ym');",
                "SELECT dolt_merge('x1', '-m', 'y2');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(100);",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y3');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'xm');",
                "SELECT dolt_merge('y1', '-m', 'x2');",
                "UPDATE t SET a = 'x edit' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x3');",
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
            name: "a foreign key onto a unique index the virtual merge base leaves out of a table whose definitions its merge bases conflict on",
            intent: "A table one merge base added with a foreign key onto a unique index of a table whose definitions conflict between the merge bases is left out of the virtual merge base, so the merge keeps it and its foreign key.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN d INT;",
                "INSERT INTO t VALUES (2, 'two', NULL, 4);",
                "CREATE UNIQUE INDEX t_d ON t (d);",
                "CREATE TABLE u (id INT PRIMARY KEY, d INT REFERENCES t (d));",
                "INSERT INTO u VALUES (1, 4);",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1', 'x');",
                "SELECT dolt_checkout('y');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1', 'y');",
                "ALTER TABLE t ALTER COLUMN c TYPE INT USING c::int;",
                "SELECT dolt_commit('-Am', 'ym');",
                "SELECT dolt_merge('x1', '-m', 'y2');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(100);",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y3');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'xm');",
                "SELECT dolt_merge('y1', '-m', 'x2');",
                "UPDATE t SET a = 'x edit' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x3');",
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
                ScriptTestAssertion {
                    query: "INSERT INTO u VALUES (2, 5);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"u\" violates foreign key constraint \"u_d_fkey\"", detail: "Key (d)=(5) is not present in table \"t\".", schema: "public", table: "u", constraint: "u_d_fkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a sequence owned by a column the virtual merge base leaves out of a table whose definitions its merge bases conflict on",
            intent: "A sequence one merge base added, owned by a column of a table whose definitions conflict between the merge bases, is left out of the virtual merge base with its state, so the merge keeps both.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN d INT;",
                "INSERT INTO t VALUES (2, 'two', NULL, 4);",
                "CREATE SEQUENCE s OWNED BY t.d;",
                "SELECT nextval('s');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1', 'x');",
                "SELECT dolt_checkout('y');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1', 'y');",
                "ALTER TABLE t ALTER COLUMN c TYPE INT USING c::int;",
                "SELECT dolt_commit('-Am', 'ym');",
                "SELECT dolt_merge('x1', '-m', 'y2');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(100);",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y3');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'xm');",
                "SELECT dolt_merge('y1', '-m', 'x2');",
                "UPDATE t SET a = 'x edit' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x3');",
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
            name: "extended statistics on a column the virtual merge base leaves out of a table whose definitions its merge bases conflict on",
            intent: "Extended statistics one merge base added on a column of a table whose definitions conflict between the merge bases are left out of the virtual merge base, so the merge keeps them.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN c INT;",
                "ALTER TABLE t ADD COLUMN d INT;",
                "INSERT INTO t VALUES (2, 'two', NULL, 4);",
                "CREATE STATISTICS st (ndistinct) ON a, d FROM t;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1', 'x');",
                "SELECT dolt_checkout('y');",
                "ALTER TABLE t ADD COLUMN c VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1', 'y');",
                "ALTER TABLE t ALTER COLUMN c TYPE INT USING c::int;",
                "SELECT dolt_commit('-Am', 'ym');",
                "SELECT dolt_merge('x1', '-m', 'y2');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(100);",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y3');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'xm');",
                "SELECT dolt_merge('y1', '-m', 'x2');",
                "UPDATE t SET a = 'x edit' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x3');",
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
            name: "a value of a NOT NULL column the virtual merge base can't convert stays out of the merged rows",
            intent: "A value that can't become the base's type of a NOT NULL column is NULL only in the virtual merge base, so the merged rows hold the tips' values.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, c INT NOT NULL);",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(100);",
                "UPDATE t SET c = 'not-an-int' WHERE id = 1;",
                "INSERT INTO t VALUES (3, 'not-an-int');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1', 'x');",
                "SELECT dolt_checkout('y');",
                "ALTER TABLE t ALTER COLUMN c TYPE BIGINT;",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1', 'y');",
                "ALTER TABLE t ALTER COLUMN c TYPE VARCHAR(100);",
                "SELECT dolt_commit('-Am', 'ym');",
                "SELECT dolt_merge('x1', '-m', 'y2');",
                "UPDATE t SET c = '7';",
                "ALTER TABLE t ALTER COLUMN c TYPE BIGINT USING c::bigint;",
                "SELECT dolt_commit('-Am', 'y3');",
                "SELECT dolt_checkout('x');",
                "UPDATE t SET c = '7';",
                "ALTER TABLE t ALTER COLUMN c TYPE BIGINT USING c::bigint;",
                "SELECT dolt_commit('-Am', 'xm');",
                "SELECT dolt_merge('y1', '-m', 'x2');",
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
                    query: "SELECT count(*) FROM t WHERE c IS NULL;",
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
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT8)],
                        rows: &[
                            &[T("1"), T("7")],
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
            name: "a volatile default the virtual merge base would fill stays NULL and marked",
            intent: "A column one merge base added with a volatile default is NULL and marked in the virtual merge base for the other base's row rather than evaluated, so the merge is the same every time, and merges cleanly where the tips agree on the cell.",
            set_up_script: &[
                "CREATE FUNCTION f() RETURNS INT VOLATILE LANGUAGE sql AS 'SELECT 7';",
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN n INT DEFAULT f();",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1', 'x');",
                "SELECT dolt_checkout('y');",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1', 'y');",
                "SELECT dolt_merge('x1', '-m', 'y2');",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y3');",
                "SELECT dolt_checkout('x');",
                "SELECT dolt_merge('y1', '-m', 'x2');",
                "UPDATE t SET a = 'x edit' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x3');",
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
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", VARCHAR), Column("n", INT4)],
                        rows: &[
                            &[T("1"), T("y edit"), T("7")],
                            &[T("2"), T("x edit"), T("7")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a nextval default the virtual merge base would fill takes no value from the sequence",
            intent: "A column one merge base added with a nextval default is NULL and marked in the virtual merge base for the other base's row, so building it takes no value from the sequence, and the tips' values of it conflict.",
            set_up_script: &[
                "CREATE SEQUENCE s;",
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN n INT DEFAULT nextval('s');",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1', 'x');",
                "SELECT dolt_checkout('y');",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1', 'y');",
                "SELECT dolt_merge('x1', '-m', 'y2');",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y3');",
                "SELECT dolt_checkout('x');",
                "SELECT dolt_merge('y1', '-m', 'x2');",
                "UPDATE t SET a = 'x edit' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x3');",
                "SET dolt_allow_commit_conflicts = on;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
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
                    query: "SELECT base_id, base_n IS NULL, our_n IS NOT NULL, their_n IS NOT NULL FROM dolt_conflicts_t ORDER BY base_id;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("2"), T("t"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
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
        ScriptTest {
            name: "a volatile default on a key column the virtual merge base would fill refuses the merge",
            intent: "A key column one merge base added with a volatile default can't be NULL and marked in the virtual merge base, since its rows would share a NULL key, so the merge refuses rather than collapse them.",
            set_up_script: &[
                "CREATE SEQUENCE s;",
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                "ALTER TABLE t ADD COLUMN k BIGINT DEFAULT nextval('s') PRIMARY KEY;",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1', 'x');",
                "SELECT dolt_checkout('y');",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1', 'y');",
                "SELECT dolt_merge('x1', '-m', 'y2');",
                "SELECT dolt_checkout('x');",
                "SELECT dolt_merge('y1', '-m', 'x2');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('y', '-m', 'merge y');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "dolt_merge of a volatile default on a key column of a converted table in a virtual merge base is not supported yet", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a random() default the virtual merge base would fill stays NULL and marked, so the tips' values conflict",
            intent: "The other merge base's row takes a NULL and marked cell in the virtual merge base rather than a random value, so the merge is the same every time and conflicts on the tips' differing values.",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(100));",
                "INSERT INTO t VALUES (1, 'seed');",
                "SELECT dolt_commit('-Am', 'c1');",
                "SELECT dolt_branch('x');",
                "SELECT dolt_branch('y');",
                "SELECT dolt_checkout('x');",
                "ALTER TABLE t ADD COLUMN n FLOAT8 DEFAULT random();",
                "SELECT dolt_commit('-Am', 'x1');",
                "SELECT dolt_branch('x1', 'x');",
                "SELECT dolt_checkout('y');",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-Am', 'y1');",
                "SELECT dolt_branch('y1', 'y');",
                "SELECT dolt_merge('x1', '-m', 'y2');",
                "UPDATE t SET a = 'y edit' WHERE id = 1;",
                "SELECT dolt_commit('-Am', 'y3');",
                "SELECT dolt_checkout('x');",
                "SELECT dolt_merge('y1', '-m', 'x2');",
                "UPDATE t SET a = 'x edit' WHERE id = 2;",
                "SELECT dolt_commit('-Am', 'x3');",
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
                    query: "SELECT base_id, base_n IS NULL, our_n IS NOT NULL, their_n IS NOT NULL FROM dolt_conflicts_t ORDER BY base_id;",
                    expected: Expected::Rows {
                        columns: &[Column("base_id", INT4), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("2"), T("t"), T("t"), T("t")],
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
