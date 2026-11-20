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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_cherry_pick.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_cherry_pick_tests() {
    run_scripts(&[
        ScriptTest {
            name: "error cases: basic validation",
            intent: "dolt_cherry_pick refuses missing, empty, and malformed arguments.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-am', 'adding row 1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('HEAD~100');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid ancestor spec", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('abcdaaaaaaaaaaaaaaaaaaaaaaaaaaaa');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "target commit not found", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: There is no cherry-pick merge to abort", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "error cases: merge commits cannot be cherry-picked",
            intent: "dolt_cherry_pick refuses a merge commit.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-am', 'adding row 1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--no-ff', 'branch1');",
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
                    query: "SELECT * FROM dolt_cherry_pick('HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cherry-picking a merge commit is not supported", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "error cases: error with staged or unstaged changes ",
            intent: "dolt_cherry_pick refuses while the working set has staged or unstaged changes.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-am', 'adding row 1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (100, 'onehundy');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot cherry-pick with uncommitted changes", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
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
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "cannot cherry-pick with uncommitted changes", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "error cases: different primary keys",
            intent: "dolt_cherry_pick refuses a commit whose table's primary key differs from the current one.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "ALTER TABLE t DROP CONSTRAINT t_pkey;",
                "SELECT dolt_commit('-am', 'adding row 1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[&[Any, T("0"), T("0"), T("0")]],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt refuses the cherry-pick with \"cannot merge because table t has different primary keys\", a limitation its merge_schema.go TODO admits, where a merge takes the primary key one side changed to when the other side kept the base's key or made the same change, converting the other side's rows to it, Appendix C \"merges and cherry-picks across a primary key change\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "basic case",
            intent: "dolt_cherry_pick applies another branch's commit's row changes as a new commit.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-am', 'adding row 1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "INSERT INTO t VALUES (2, 'two');",
                "SELECT dolt_commit('-am', 'adding row 2');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit2', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                            &[T("2"), T("two")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_commit_ancestors WHERE commit_hash = dolt_hashof('HEAD');",
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
            name: "keyless table",
            intent: "dolt_cherry_pick applies a commit's changes to a table without a primary key.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE keyless (id int, name varchar(10));",
                "SELECT dolt_commit('-Am', 'create table keyless on main');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO keyless VALUES (1,'1'), (2,'3');",
                "SELECT dolt_commit('-am', 'insert rows into keyless table on branch1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('branch1');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
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
            name: "schema change: CREATE TABLE",
            intent: "dolt_cherry_pick of a commit that creates a table creates it.",
            set_up_script: &[
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "CREATE TABLE table_a (pk bigint PRIMARY KEY, v varchar(10));",
                "INSERT INTO table_a VALUES (11, 'aa'), (22, 'ab');",
                "SELECT dolt_commit('-Am', 'create table table_a');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_commit_ancestors WHERE commit_hash = dolt_hashof('HEAD');",
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
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("table_a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM table_a;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v", VARCHAR)],
                        rows: &[
                            &[T("11"), T("aa")],
                            &[T("22"), T("ab")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema change: DROP TABLE",
            intent: "dolt_cherry_pick of a commit that drops a table drops it.",
            set_up_script: &[
                "CREATE TABLE dropme (pk bigint PRIMARY KEY, v varchar(10));",
                "INSERT INTO dropme VALUES (11, 'aa'), (22, 'ab');",
                "SELECT dolt_commit('-Am', 'create table dropme');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "DROP TABLE dropme;",
                "SELECT dolt_commit('-Am', 'drop table dropme');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("dropme")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
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
            name: "schema change: ALTER TABLE ADD COLUMN",
            intent: "dolt_cherry_pick of a commit that adds a column adds it.",
            set_up_script: &[
                "CREATE TABLE test(pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'create table test on main');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "ALTER TABLE test ADD COLUMN v VARCHAR(100);",
                "SELECT dolt_commit('-am', 'add column v to test on branch1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("pk")],
                            &[T("v")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns lists the table's columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema change: ALTER TABLE DROP COLUMN",
            intent: "dolt_cherry_pick of a commit that drops a column drops it.",
            set_up_script: &[
                "CREATE TABLE test(pk int PRIMARY KEY, v varchar(100));",
                "SELECT dolt_commit('-Am', 'create table test on main');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "ALTER TABLE test DROP COLUMN v;",
                "SELECT dolt_commit('-am', 'drop column v from test on branch1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("pk")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns lists the table's columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "schema change: ALTER TABLE RENAME COLUMN",
            intent: "dolt_cherry_pick of a commit that renames a column renames it.",
            set_up_script: &[
                "CREATE TABLE test(pk int PRIMARY KEY, v1 varchar(100));",
                "SELECT dolt_commit('-Am', 'create table test on main');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "ALTER TABLE test RENAME COLUMN v1 TO v2;",
                "SELECT dolt_commit('-am', 'rename column v1 to v2 in test on branch1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("pk")],
                            &[T("v2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns lists the table's columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "abort (@@autocommit=0)",
            intent: "dolt_cherry_pick --abort in a transaction block restores the state before a conflicted cherry-pick.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "UPDATE t SET v='uno' WHERE pk=1;",
                "SELECT dolt_commit('-Am', 'updating row 1 -> uno');",
                "ALTER TABLE t DROP COLUMN v;",
                "SELECT dolt_commit('-am', 'drop column v');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(dolt_hashof('branch1'));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_v, our_pk, dolt_our_diff_type, their_pk, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_v", VARCHAR), Column("our_pk", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("uno"), T("1"), T("modified"), T("1"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "abort (@@autocommit=1)",
            intent: "dolt_cherry_pick --abort restores the state before a conflicted cherry-pick.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "UPDATE t SET v='uno' WHERE pk=1;",
                "SELECT dolt_commit('-Am', 'updating row 1 -> uno');",
                "ALTER TABLE t DROP COLUMN v;",
                "SELECT dolt_commit('-am', 'drop column v');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(dolt_hashof('branch1'));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_v, our_pk, dolt_our_diff_type, their_pk, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_v", VARCHAR), Column("our_pk", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("uno"), T("1"), T("modified"), T("1"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict resolution (@@autocommit=0)",
            intent: "A conflicted cherry-pick in a transaction block leaves conflicts that resolving and committing finishes.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "UPDATE t SET v='uno' WHERE pk=1;",
                "SELECT dolt_commit('-Am', 'updating row 1 -> uno');",
                "ALTER TABLE t DROP COLUMN v;",
                "SELECT dolt_commit('-am', 'drop column v');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(dolt_hashof('branch1'));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("f"), T("modified")],
                            &[T("public.t"), T("f"), T("conflict")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_v, our_pk, dolt_our_diff_type, their_pk, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_v", VARCHAR), Column("our_pk", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("uno"), T("1"), T("modified"), T("1"), T("modified")],
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
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
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
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'committing cherry-pick');",
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
                    query: "SELECT count(*) FROM dolt_commit_ancestors WHERE commit_hash = dolt_hashof('HEAD');",
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
            name: "conflict resolution (@@autocommit=1)",
            intent: "A conflicted cherry-pick leaves conflicts that resolving and committing finishes.",
            set_up_script: &[
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));",
                "SELECT dolt_commit('-Am', 'creating table t');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'inserting row 1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "UPDATE t SET c1='uno' WHERE pk=1;",
                "SELECT dolt_commit('-Am', 'updating row 1 -> uno');",
                "UPDATE t SET c1='ein' WHERE pk=1;",
                "SELECT dolt_commit('-Am', 'updating row 1 -> ein');",
                "SELECT set_config('test.commit2', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT dolt_reset('--hard', current_setting('test.commit1', TRUE));",
            ],
            assertions: &[
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
                    query: "SELECT * FROM t;",
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
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit2', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_c1, our_pk, our_c1, dolt_their_diff_type, their_pk, their_c1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_c1", VARCHAR), Column("our_pk", INT4), Column("our_c1", VARCHAR), Column("dolt_their_diff_type", TEXT), Column("their_pk", INT4), Column("their_c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("uno"), T("1"), T("one"), T("modified"), T("1"), T("ein")],
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", VARCHAR)],
                        rows: &[
                            &[T("1"), T("ein")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'committing cherry-pick');",
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
                    query: "SELECT count(*) FROM dolt_commit_ancestors WHERE commit_hash = dolt_hashof('HEAD');",
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
            name: "abort (@@autocommit=1) with ignored table",
            intent: "dolt_cherry_pick --abort leaves an ignored table alone.",
            set_up_script: &[
                "INSERT INTO dolt_ignore VALUES ('generated_*', TRUE);",
                "CREATE TABLE generated_foo (pk int PRIMARY KEY);",
                "CREATE TABLE generated_bar (pk int PRIMARY KEY);",
                "INSERT INTO generated_foo VALUES (1);",
                "INSERT INTO generated_bar VALUES (1);",
                "SET dolt_allow_commit_conflicts = 1;",
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_add('--force', 'generated_bar');",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "UPDATE t SET v='uno' WHERE pk=1;",
                "SELECT dolt_commit('-Am', 'updating row 1 -> uno');",
                "ALTER TABLE t DROP COLUMN v;",
                "SELECT dolt_commit('-am', 'drop column v');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(dolt_hashof('branch1'));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_v, our_pk, dolt_our_diff_type, their_pk, dolt_their_diff_type FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_v", VARCHAR), Column("our_pk", INT4), Column("dolt_our_diff_type", TEXT), Column("their_pk", INT4), Column("dolt_their_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("uno"), T("1"), T("modified"), T("1"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO generated_foo VALUES (2);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--abort');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status_ignored;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.generated_foo"), T("f"), T("new table"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generated_foo;",
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
        ScriptTest {
            name: "cherry-pick --continue: successful conflict resolution workflow",
            intent: "After a conflicted cherry-pick's conflicts are resolved and staged, dolt_cherry_pick --continue makes the commit.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t VALUES (1, 'branch1_value');",
                "SELECT dolt_commit('-am', 'add row from branch1', '--author', 'Test User <test@example.com>', '--date', '2022-01-01T12:00:00');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 'main_value');",
                "SELECT dolt_commit('-am', 'add row from main');",
                "SET dolt_committer_date = '2022-01-01T12:00:00';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT our_pk, our_v, their_pk, their_v FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("our_pk", INT4), Column("our_v", VARCHAR), Column("their_pk", INT4), Column("their_v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("main_value"), T("1"), T("branch1_value")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = 'resolved_value' WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
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
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved_value")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT author, author_email, author_date, committer, email, message, date FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("author", TEXT), Column("author_email", TEXT), Column("author_date", TIMESTAMPTZ), Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT), Column("date", TIMESTAMPTZ)],
                        rows: &[
                            &[T("Test User"), T("test@example.com"), T("2022-01-01 12:00:00+00"), T("postgres"), T("postgres@localhost"), T("add row from branch1"), T("2022-01-01 12:00:00+00")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root@localhost; ours as the session's user, postgres")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick --continue not in a cherry-pick state",
            intent: "dolt_cherry_pick --continue fails when no cherry-pick is in progress.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-am', 'add row from branch1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: There is no cherry-pick merge to continue", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick --continue: multiple table conflicts",
            intent: "dolt_cherry_pick --continue succeeds once conflicts in several tables are resolved.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, v varchar(100));",
                "CREATE TABLE t2 (pk int PRIMARY KEY, v varchar(100));",
                "SELECT dolt_commit('-Am', 'create tables');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t1 VALUES (1, 'branch1_t1');",
                "INSERT INTO t2 VALUES (1, 'branch1_t2');",
                "SELECT dolt_commit('-am', 'add rows from branch1', '--author', 'Branch User <branch@example.com>', '--date', '2022-02-01T10:30:00');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t1 VALUES (1, 'main_t1');",
                "INSERT INTO t2 VALUES (1, 'main_t2');",
                "SELECT dolt_commit('-am', 'add rows from main');",
                "SET dolt_committer_date = '2022-02-01T10:30:00Z';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("2"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT \"table\" FROM dolt_conflicts ORDER BY \"table\";",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT)],
                        rows: &[
                            &[T("public.t1")],
                            &[T("public.t2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t1 SET v = 'resolved_t1' WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t2 SET v = 'resolved_t2' WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t2;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved_t1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved_t2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT author, author_email, author_date, committer, email, message, date FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("author", TEXT), Column("author_email", TEXT), Column("author_date", TIMESTAMPTZ), Column("committer", TEXT), Column("email", TEXT), Column("message", TEXT), Column("date", TIMESTAMPTZ)],
                        rows: &[
                            &[T("Branch User"), T("branch@example.com"), T("2022-02-01 10:30:00+00"), T("postgres"), T("postgres@localhost"), T("add rows from branch1"), T("2022-02-01 10:30:00+00")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Setup("Dolt's harness commits as root@localhost; ours as the session's user, postgres")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick --continue: mutually exclusive with --abort",
            intent: "dolt_cherry_pick refuses --continue and --abort together.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t VALUES (1, 'branch1_value');",
                "SELECT dolt_commit('-am', 'add row from branch1');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 'main_value');",
                "SELECT dolt_commit('-am', 'add row from main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--continue', '--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: --continue and --abort are mutually exclusive", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--abort', '--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: --continue and --abort are mutually exclusive", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: constraint violations only (no merge state)",
            intent: "A cherry-pick that only breaks constraints records the violations without a merge in progress.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t VALUES (1, 'forbidden');",
                "SELECT dolt_commit('-am', 'add forbidden value');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD CONSTRAINT chk_not_forbidden CHECK (v != 'forbidden');",
                "SELECT dolt_commit('-am', 'add check constraint');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("check constraint"), T("1"), T("forbidden")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = 'allowed' WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
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
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("allowed")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick --continue: with both conflicts and constraint violations",
            intent: "dolt_cherry_pick --continue finishes once both conflicts and constraint violations are resolved.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "INSERT INTO t VALUES (1, 'initial');",
                "SELECT dolt_commit('-Am', 'create table t and row');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "-- On branch1, modify existing row and add new row with value that will violate constraint;",
                "UPDATE t SET v = 'branch1_value' WHERE pk = 1;",
                "INSERT INTO t VALUES (2, 'forbidden');",
                "SELECT dolt_commit('-am', 'modify row 1 and add row 2 with forbidden value');",
                "SELECT set_config('test.commit1', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "-- On main, change row 1 to create conflict and add constraint;",
                "UPDATE t SET v = 'main_value' WHERE pk = 1;",
                "ALTER TABLE t ADD CONSTRAINT chk_not_forbidden CHECK (v != 'forbidden');",
                "SELECT dolt_commit('-am', 'main changes and add constraint');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.commit1', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, v FROM dolt_constraint_violations_t;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("check constraint"), T("2"), T("forbidden")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("1"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = 'resolved_value' WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_t;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE pk = 2;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_constraint_violations_t;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick('--continue');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", VARCHAR)],
                        rows: &[
                            &[T("1"), T("resolved_value")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cherry-pick: records current session as committer separately from author",
            intent: "A cherry-picked commit keeps its author and date and records the current session as committer.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v varchar(100));",
                "SELECT dolt_commit('-Am', 'create table t');",
                "SELECT * FROM dolt_checkout('-b', 'branch1');",
                "INSERT INTO t VALUES (1, 'one');",
                "SELECT dolt_commit('-am', 'add row 1', '--author', 'Original Author <orig@example.com>');",
                "SELECT set_config('test.cherry', (dolt_hashof('HEAD'))::text, FALSE);",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_committer_name = 'Cherry Picker';",
                "SET dolt_committer_email = 'picker@example.com';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_cherry_pick(current_setting('test.cherry', TRUE));",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("data_conflicts", INT8), Column("schema_conflicts", INT8), Column("constraint_violations", INT8)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT author, author_email, committer, email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("author", TEXT), Column("author_email", TEXT), Column("committer", TEXT), Column("email", TEXT)],
                        rows: &[
                            &[T("Original Author"), T("orig@example.com"), T("Cherry Picker"), T("picker@example.com")],
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
