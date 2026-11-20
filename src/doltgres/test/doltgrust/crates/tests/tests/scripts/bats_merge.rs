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

//! Dolt's integration-tests/bats/merge.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_merge() {
    run_scripts(&[
        ScriptTest {
            name: "merge: db collation ff merge",
            intent: "Merging a database's collation change that is already merged changes nothing.",
            set_up_script: &[
                "CREATE DATABASE colldb;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("Everything up-to-date")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: db collation non ff merge",
            intent: "The same database collation change on both sides merges to nothing new.",
            set_up_script: &[
                "CREATE DATABASE colldb;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("Everything up-to-date")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("MySQL-only: a database's default collation can't change in Postgres"),
            ..S
        },
        ScriptTest {
            name: "merge: db collation merge conflict",
            intent: "Different database collation changes on each side are a conflict.",
            set_up_script: &[
                "CREATE DATABASE colldb;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "database collation conflict", message_contains: true, ..E }),
                    ..A
                },
            ],
            skip: Some("MySQL-only: a database's default collation can't change in Postgres"),
            ..S
        },
        ScriptTest {
            name: "merge: unresolved FKs not dropped on merge (issue #5531)",
            intent: "A merge keeps a foreign key made with the checks off.",
            set_up_script: &[
                "SET session_replication_role = replica;",
                "CREATE TABLE operations (id int PRIMARY KEY);",
                "CREATE TABLE diff_summaries (id int PRIMARY KEY, op_id_fk int, FOREIGN KEY (op_id_fk) REFERENCES operations (id));",
                "SET session_replication_role = origin;",
                "SELECT dolt_commit('-Am', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'base');",
                "SELECT dolt_branch('two');",
                "SELECT * FROM dolt_checkout('-b', 'one');",
                "INSERT INTO operations VALUES (1);",
                "SELECT dolt_commit('-Am', 'insert operations 1');",
                "SELECT * FROM dolt_checkout('two');",
                "INSERT INTO operations VALUES (2);",
                "SELECT dolt_commit('-Am', 'insert operations 2');",
                "SELECT * FROM dolt_checkout('-b', 'merged');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--no-edit', 'one');",
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
                    query: "SELECT pg_get_constraintdef(oid) AS definition FROM pg_constraint WHERE conrelid = 'diff_summaries'::regclass AND contype = 'f';",
                    expected: Expected::Rows {
                        columns: &[Column("definition", TEXT)],
                        rows: &[
                            &[T("FOREIGN KEY (op_id_fk) REFERENCES operations(id)")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so pg_constraint shows the foreign key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: three-way merge with longer key on both left and right",
            intent: "Primary keys lengthened differently on each side fail to merge.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "CREATE TABLE t1 (a int, b int, c int, PRIMARY KEY (a, b));",
                "INSERT INTO t1 VALUES (1, 1, 1), (2, 2, 2);",
                "SELECT dolt_commit('-Am', 'new table');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT * FROM dolt_checkout('b1');",
                "ALTER TABLE t1 ADD COLUMN d int NOT NULL DEFAULT 4;",
                "ALTER TABLE t1 DROP CONSTRAINT t1_pkey;",
                "ALTER TABLE t1 ADD PRIMARY KEY (a, b, d);",
                "UPDATE t1 SET d = 5;",
                "SELECT dolt_commit('-Am', 'added a column to the primary key with value 5');",
                "SELECT * FROM dolt_checkout('b2');",
                "ALTER TABLE t1 ADD COLUMN d int NOT NULL DEFAULT 4;",
                "ALTER TABLE t1 DROP CONSTRAINT t1_pkey;",
                "ALTER TABLE t1 ADD PRIMARY KEY (a, b, d);",
                "UPDATE t1 SET d = 6;",
                "SELECT dolt_commit('-Am', 'added a column to the primary key with value 6');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("1"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b2');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt fails the merge, where both sides' identical primary key change converges and rows each side moved to another key are two rows, as in any merge of rows whose keys both sides changed, Appendix C \"merges and cherry-picks across a primary key change\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a, b, d;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4), Column("d", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("5")],
                            &[T("1"), T("1"), T("1"), T("6")],
                            &[T("2"), T("2"), T("2"), T("5")],
                            &[T("2"), T("2"), T("2"), T("6")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Differs("Dolt fails the merge, where both sides' identical primary key change converges and rows each side moved to another key are two rows, as in any merge of rows whose keys both sides changed, Appendix C \"merges and cherry-picks across a primary key change\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: 3way merge doesn't stomp working changes",
            intent: "A --no-commit three-way merge stages its changes beside another table's, and committing it keeps both sides' commits in the log.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test1 VALUES (0, 1, 2);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'add pk 0 to test1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test1 VALUES (1, 2, 3);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'add pk 1 to test1');",
                "INSERT INTO test2 VALUES (0, 1, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('merge_branch', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge successful")],
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
                            &[T("public.test1"), T("t"), T("modified")],
                            &[T("public.test2"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging, source, target FROM dolt_merge_status;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT)],
                        rows: &[
                            &[T("t"), T("merge_branch"), T("refs/heads/main")],
                        ],
                        tag: "SELECT 1",
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
                    query: "SELECT length(dolt_commit('-m', 'squash merge')) > 0 AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log WHERE message = 'add pk 0 to test1';",
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
                    query: "SELECT count(*) FROM dolt_log WHERE message = 'add pk 1 to test1';",
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
            name: "merge: --abort restores working changes",
            intent: "Aborting a conflicting merge restores another table's uncommitted changes.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test1 VALUES (0, 10, 10), (1, 11, 11);",
                "SELECT dolt_commit('-am', 'added rows to test1 on main');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO test1 VALUES (0, 20, 20), (1, 21, 21);",
                "SELECT dolt_commit('-am', 'added rows to test1 on other');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test2 VALUES (9, 9, 9);",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '--no-commit');",
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
                    query: "SELECT * FROM test2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("9"), T("9"), T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: --abort leaves clean working, staging roots",
            intent: "Aborting a --no-commit merge leaves a clean working set and no merge.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test1 VALUES (1, 10, 10);",
                "SELECT dolt_commit('-am', 'added rows to test1 on main');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO test1 VALUES (2, 20, 20);",
                "SELECT dolt_commit('-am', 'added rows to test1 on other');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging, source, target FROM dolt_merge_status;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT)],
                        rows: &[
                            &[T("t"), T("other"), T("refs/heads/main")],
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
                            &[T("public.test1"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
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
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
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
                        ],
                        tag: "SELECT 0",
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
            ],
            ..S
        },
        ScriptTest {
            name: "merge: squash merge",
            intent: "A squash merge stages the branch's changes beside another table's, and the squashed commit is not in the log.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test1 VALUES (0, 1, 2);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'add pk 0 to test1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test1 VALUES (1, 2, 3);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'add pk 1 to test1');",
                "INSERT INTO test2 VALUES (0, 1, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--squash', 'merge_branch', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge successful")],
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
                            &[T("public.test1"), T("t"), T("modified")],
                            &[T("public.test2"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
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
                    query: "SELECT length(dolt_commit('-m', 'squash merge')) > 0 AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log WHERE message = 'add pk 1 to test1';",
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
                    query: "SELECT count(*) FROM dolt_log WHERE message = 'add pk 0 to test1';",
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
            name: "merge: can merge commit spec with ancestor spec",
            intent: "A merge of a branch's parent commit fast-forwards to it.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test1 VALUES (0, 1, 2);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'add pk 0 to test1');",
                "INSERT INTO test1 VALUES (1, 2, 3);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'add pk 1 to test1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('merge_branch~');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("1"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test1 WHERE pk = 1;",
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
            ],
            ..S
        },
        ScriptTest {
            name: "merge: dolt commit fails on table with conflict",
            intent: "A merge with conflicts cannot commit until forced, and the merge stays in progress.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test1 VALUES (0, 1, 1);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'add pk 0 = 1,1 to test1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test1 VALUES (0, 2, 2);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'add pk 0 = 2,2 to test1');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('merge_branch', '-m', 'merge_branch');",
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
                    query: "SELECT dolt_add('test1');",
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
                    query: "SELECT dolt_commit('-am', 'can''t commit with conflicts');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: the table(s) test1 are in conflict", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_merging, source, target FROM dolt_merge_status;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("target", TEXT)],
                        rows: &[
                            &[T("t"), T("merge_branch"), T("refs/heads/main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('--force', '-am', 'force commit with conflicts')) > 0 AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
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
            name: "merge: dolt commit fails with unmerged tables in working set",
            intent: "A --no-commit merge with conflicts cannot commit.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test1 VALUES (0, 1, 1);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'add pk 0 = 1,1 to test1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test1 VALUES (0, 2, 2);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'add pk 0 = 2,2 to test1');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('merge_branch', '--no-commit');",
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
                    query: "SELECT dolt_commit('-m', 'create a merge commit');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: the table(s) test1 are in conflict", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: ff merge doesn't stomp working changes",
            intent: "A fast-forward merge keeps another table's uncommitted changes.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test1 VALUES (0, 1, 2);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'modify test1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test2 VALUES (0, 1, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('merge_branch');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("1"), T("0"), T("merge successful")],
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
                            &[T("public.test2"), T("f"), T("modified")],
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
            name: "merge: no-ff merge",
            intent: "A no-ff merge makes a merge commit with its message.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test1 VALUES (0, 1, 2);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'modify test1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('merge_branch', '--no-ff', '-m', 'no-ff merge');",
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
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("no-ff merge")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: no-ff merge doesn't stomp working changes and doesn't fast forward",
            intent: "A no-ff merge keeps another table's uncommitted changes and makes a merge commit.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test1 VALUES (0, 1, 2);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'modify test1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test2 VALUES (0, 1, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('merge_branch', '--no-ff', '-m', 'no-ff merge');",
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
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("no-ff merge")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: 3way merge rejected when working changes touch same tables",
            intent: "A three-way merge refuses to overwrite a table with uncommitted changes.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test1 VALUES (0, 1, 2);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'add pk 0 to test1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test2 VALUES (0, 1, 2);",
                "SELECT dolt_add('test2');",
                "SELECT dolt_commit('-m', 'add pk 0 to test2');",
                "INSERT INTO test1 VALUES (1, 2, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('merge_branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: local changes would be stomped by merge:\n\tpublic.test1\n Please commit your changes before you merge.", ..E }),
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: ff merge rejected when working changes touch same tables",
            intent: "A fast-forward merge refuses to overwrite a table with uncommitted changes.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test1 VALUES (0, 1, 2);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'modify test1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test1 VALUES (1, 2, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test1"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('merge_branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: local changes would be stomped by merge:\n\tpublic.test1\n Please commit your changes before you merge.", ..E }),
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: Add tables with same schema on two branches, merge",
            intent: "A table added with the same schema on each side merges with both sides' rows.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT dolt_branch('other');",
                "CREATE TABLE quiz (pk int PRIMARY KEY);",
                "INSERT INTO quiz VALUES (10), (11), (12);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added table quiz on main');",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE TABLE quiz (pk int PRIMARY KEY);",
                "INSERT INTO quiz VALUES (20), (21), (22);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added table quiz on other');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
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
                    query: "SELECT * FROM quiz ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("11")],
                            &[T("12")],
                            &[T("20")],
                            &[T("21")],
                            &[T("22")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: Add views on two branches, merge",
            intent: "Views added on each side both exist after the merge.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT dolt_branch('other');",
                "CREATE VIEW pkpk AS SELECT pk * pk FROM test1;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added view on table test1');",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE VIEW c1c1 AS SELECT c1 * c1 FROM test2;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added view on table test2');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name::text FROM information_schema.views WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("c1c1")],
                            &[T("pkpk")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt_schemas holds MySQL's views, where Postgres keeps them in pg_class, which information_schema.views reads")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: Add views on two branches, merge with stored procedure",
            intent: "Views added on each side both exist after the merge.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT dolt_branch('other');",
                "CREATE VIEW pkpk AS SELECT pk * pk FROM test1;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added view on table test1');",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE VIEW c1c1 AS SELECT c1 * c1 FROM test2;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added view on table test2');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name::text FROM information_schema.views WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("c1c1")],
                            &[T("pkpk")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt_schemas holds MySQL's views, where Postgres keeps them in pg_class, which information_schema.views reads")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: Add views on two branches, merge without conflicts",
            intent: "Views added on each side both exist after a committed merge.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT dolt_branch('other');",
                "CREATE VIEW pkpk AS SELECT pk * pk FROM test1;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added view on table test1');",
                "SELECT * FROM dolt_checkout('other');",
                "CREATE VIEW c1c1 AS SELECT c1 * c1 FROM test2;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added view on table test2');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt skips it (key collision in dolt_schemas); Doltgrust runs it as written")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name::text FROM information_schema.views WHERE table_schema = 'public' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("c1c1")],
                            &[T("pkpk")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt_schemas holds MySQL's views, where Postgres keeps them in pg_class, which information_schema.views reads")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: unique index conflict",
            intent: "Rows each side added with the same unique value are a constraint violation that leaves the table unmerged.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int); CREATE UNIQUE INDEX ON test (c0);",
                "INSERT INTO test VALUES (0, 0);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'setup');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "INSERT INTO test VALUES (2, 19);",
                "SELECT dolt_commit('-am', 'added row');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (1, 19);",
                "SELECT dolt_commit('-am', 'added row');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("constraint violation")],
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
            name: "merge: composite unique index conflict",
            intent: "Updates on each side that make a composite unique value repeat are violations of both rows.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int, c1 int); CREATE UNIQUE INDEX ON test (c0, c1);",
                "INSERT INTO test VALUES (0, 0, 0);",
                "INSERT INTO test VALUES (1, 11, 2);",
                "INSERT INTO test VALUES (2, 1, 22);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'setup');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "UPDATE test SET c0 = 1 WHERE c0 = 11;",
                "SELECT dolt_commit('-am', 'added row');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE test SET c1 = 2 WHERE c1 = 22;",
                "SELECT dolt_commit('-am', 'added row');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.test"), T("2")],
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
            name: "merge: composite indexes should be consistent post-merge",
            intent: "A composite index finds only the merged values of a row both sides changed.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "CREATE TABLE test (id int PRIMARY KEY, c0 int, c1 int); CREATE INDEX idx_c0_c1 ON test (c0, c1);",
                "INSERT INTO test VALUES (1, 0, 0);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial data');",
                "SELECT dolt_branch('right');",
                "UPDATE test SET c0 = 1;",
                "SELECT dolt_commit('-am', 'left commit');",
                "SELECT * FROM dolt_checkout('right');",
                "UPDATE test SET c1 = 1;",
                "SELECT dolt_commit('-am', 'right commit');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('right', '-m', 'merge');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test WHERE c0 = 1 AND c1 = 0;",
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
                    query: "SELECT count(*) FROM test WHERE c0 = 0 AND c1 = 1;",
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
                    query: "SELECT count(*) FROM test WHERE c0 = 1 AND c1 = 1;",
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
            name: "merge: merge a branch with a new table",
            intent: "A table added on one side merges with rows added on the other.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT dolt_branch('feature-branch');",
                "INSERT INTO test2 VALUES (0, 0, 0);",
                "INSERT INTO test2 VALUES (1, 1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'add data to test 2');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "CREATE TABLE test3 (a int PRIMARY KEY);",
                "INSERT INTO test3 VALUES (0), (1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table test3');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'merge feature-branch');",
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
                    query: "SELECT * FROM test3 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: merge a branch that deletes a table",
            intent: "A table dropped on one side is gone after the merge, beside the other table's rows from both sides.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT dolt_branch('feature-branch');",
                "INSERT INTO test1 VALUES (0, 0, 0);",
                "INSERT INTO test1 VALUES (1, 1, 1);",
                "SELECT dolt_commit('-am', 'add data to test1');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "INSERT INTO test1 VALUES (2, 2, 2);",
                "DROP TABLE test2;",
                "SELECT dolt_commit('-am', 'add data to test1, drop test2');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('feature-branch', '-m', 'merge feature-branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_regclass('test2') IS NOT NULL AS exists;",
                    expected: Expected::Rows {
                        columns: &[Column("exists", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: merge branch with table that was deleted",
            intent: "Merging a branch that kept a table we dropped leaves it dropped.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "INSERT INTO test2 VALUES (0, 0, 0);",
                "INSERT INTO test2 VALUES (1, 1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'add data to test2');",
                "SELECT dolt_branch('feature-branch');",
                "DROP TABLE test2;",
                "SELECT dolt_commit('-am', 'drop table test2');",
                "INSERT INTO test1 VALUES (2, 2, 2);",
                "INSERT INTO test1 VALUES (3, 3, 3);",
                "SELECT dolt_commit('-am', 'add data to test1');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "INSERT INTO test1 VALUES (0, 0, 0);",
                "INSERT INTO test1 VALUES (1, 1, 1);",
                "SELECT dolt_commit('-am', 'add data to test1');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('feature-branch', '-m', 'merge feature-branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_regclass('test2') IS NOT NULL AS exists;",
                    expected: Expected::Rows {
                        columns: &[Column("exists", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2")],
                            &[T("3"), T("3"), T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: merge a branch that edits a deleted table",
            intent: "Rows added to a table the other side dropped are a schema conflict.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "INSERT INTO test2 VALUES (0, 0, 0);",
                "INSERT INTO test2 VALUES (1, 1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'add data to test2');",
                "SELECT dolt_branch('feature-branch');",
                "DROP TABLE test2;",
                "SELECT dolt_commit('-am', 'drop table test2');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "INSERT INTO test2 VALUES (2, 2, 2);",
                "SELECT dolt_commit('-am', 'add data to test2');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
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
                            &[T("public.test2"), T("cannot merge a table deletion with data modification")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT strpos(their_schema, 'c1 integer') > 0 AS their, strpos(base_schema, 'c1 integer') > 0 AS base FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("their", BOOL), Column("base", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_conflicts shows each side's schema as pg_dump's statements for the table, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: merge a branch that edits the schema of a deleted table",
            intent: "A column dropped from a table the other side dropped is a schema conflict.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "INSERT INTO test2 VALUES (0, 0, 0);",
                "INSERT INTO test2 VALUES (1, 1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'add data to test2');",
                "SELECT dolt_branch('feature-branch');",
                "DROP TABLE test2;",
                "SELECT dolt_commit('-am', 'drop table test2');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "ALTER TABLE test2 DROP COLUMN c2;",
                "SELECT dolt_commit('-am', 'add data to test2');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
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
                            &[T("public.test2"), T("cannot merge a table deletion with schema modification")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT strpos(their_schema, 'c1 integer') > 0 AS their, strpos(base_schema, 'c2 integer') > 0 AS base FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("their", BOOL), Column("base", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_conflicts shows each side's schema as pg_dump's statements for the table, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: merge a branch that deletes an edited table",
            intent: "Dropping a table the other side added rows to is a schema conflict.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "INSERT INTO test2 VALUES (0, 0, 0);",
                "INSERT INTO test2 VALUES (1, 1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'add data to test2');",
                "SELECT dolt_branch('feature-branch');",
                "INSERT INTO test2 VALUES (2, 2, 2);",
                "SELECT dolt_commit('-am', 'add data to test2');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "DROP TABLE test2;",
                "SELECT dolt_commit('-am', 'drop table test2');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
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
                            &[T("public.test2"), T("cannot merge a table deletion with data modification")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT strpos(our_schema, 'c1 integer') > 0 AS our, strpos(base_schema, 'c1 integer') > 0 AS base FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("our", BOOL), Column("base", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_conflicts shows each side's schema as pg_dump's statements for the table, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: merge a branch that deletes a schema-edited table",
            intent: "Dropping a table the other side changed the schema of is a schema conflict.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "INSERT INTO test2 VALUES (0, 0, 0);",
                "INSERT INTO test2 VALUES (1, 1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'add data to test2');",
                "SELECT dolt_branch('feature-branch');",
                "ALTER TABLE test2 DROP COLUMN c2;",
                "SELECT dolt_commit('-am', 'add data to test2');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "DROP TABLE test2;",
                "SELECT dolt_commit('-am', 'drop table test2');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
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
                            &[T("public.test2"), T("cannot merge a table deletion with schema modification")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT strpos(our_schema, 'c1 integer') > 0 AS our, strpos(base_schema, 'c2 integer') > 0 AS base FROM dolt_schema_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("our", BOOL), Column("base", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("dolt_schema_conflicts shows each side's schema as pg_dump's statements for the table, Appendix C \"dolt_patch statements\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: merge a branch that deletes a deleted table",
            intent: "A table dropped on both sides merges as dropped.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "INSERT INTO test2 VALUES (0, 0, 0);",
                "INSERT INTO test2 VALUES (1, 1, 1);",
                "SELECT dolt_add('-A');",
                "SELECT dolt_commit('-am', 'add data to test2');",
                "SELECT dolt_branch('feature-branch');",
                "INSERT INTO test2 VALUES (2, 2, 2);",
                "SELECT dolt_commit('-am', 'add data to test2');",
                "DROP TABLE test2;",
                "SELECT dolt_commit('-am', 'drop test2');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "DROP TABLE test2;",
                "SELECT dolt_commit('-am', 'drop table test2');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-m', 'merge feature-branch');",
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
                    query: "SELECT to_regclass('test1') IS NOT NULL AS exists;",
                    expected: Expected::Rows {
                        columns: &[Column("exists", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_regclass('test2') IS NOT NULL AS exists;",
                    expected: Expected::Rows {
                        columns: &[Column("exists", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: non-violating merge succeeds when violations already exist",
            intent: "A merge with no new violations keeps the committed violations it started with.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "CREATE TABLE parent (pk int PRIMARY KEY, col1 int);",
                "CREATE TABLE child (pk int PRIMARY KEY, parent_fk int, FOREIGN KEY (parent_fk) REFERENCES parent (pk));",
                "CREATE TABLE other (pk int);",
                "INSERT INTO parent VALUES (1, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table with data');",
                "SELECT dolt_branch('right');",
                "DELETE FROM parent WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'delete pk = 1 from left');",
                "SELECT * FROM dolt_checkout('right');",
                "INSERT INTO child VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'add child of 1 to right');",
                "SELECT dolt_branch('other');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO other VALUES (1);",
                "SELECT dolt_commit('-am', 'non-fk insert');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
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
                    query: "SELECT length(dolt_commit('-afm', 'commit constraint violations')) > 0 AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '--no-commit');",
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
                    query: "SELECT dolt_violation_type, pk, parent_fk FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("parent_fk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1"), T("1")],
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
            name: "merge: non-conflicting / non-violating merge succeeds when conflicts and violations already exist",
            intent: "A merge with no new conflicts or violations keeps the committed ones it started with and adds its rows.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "CREATE TABLE parent (pk int PRIMARY KEY, col1 int);",
                "CREATE TABLE child (pk int PRIMARY KEY, parent_fk int, FOREIGN KEY (parent_fk) REFERENCES parent (pk));",
                "INSERT INTO parent VALUES (1, 1), (2, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table with data');",
                "SELECT dolt_branch('other');",
                "SELECT dolt_branch('other2');",
                "UPDATE parent SET col1 = 2 WHERE pk = 1;",
                "DELETE FROM parent WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'updating col1 to 2 and remove pk = 2');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE parent SET col1 = 3 WHERE pk = 1;",
                "INSERT INTO child VALUES (1, 2);",
                "SELECT dolt_commit('-am', 'updating col1 to 3 and adding child of pk 2');",
                "SELECT * FROM dolt_checkout('other2');",
                "INSERT INTO parent VALUES (3, 1);",
                "SELECT dolt_commit('-am', 'insert parent with pk 3');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("parent_fk", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_col1, base_pk, our_col1, our_pk, their_col1, their_pk FROM dolt_conflicts_parent;",
                    expected: Expected::Rows {
                        columns: &[Column("base_col1", INT4), Column("base_pk", INT4), Column("our_col1", INT4), Column("our_pk", INT4), Column("their_col1", INT4), Column("their_pk", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("2"), T("1"), T("3"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, parent_fk FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("parent_fk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-afm', 'committing merge conflicts')) > 0 AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other2', '--no-commit');",
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
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("3"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("parent_fk", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_col1, base_pk, our_col1, our_pk, their_col1, their_pk FROM dolt_conflicts_parent;",
                    expected: Expected::Rows {
                        columns: &[Column("base_col1", INT4), Column("base_pk", INT4), Column("our_col1", INT4), Column("our_pk", INT4), Column("their_col1", INT4), Column("their_pk", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("2"), T("1"), T("3"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, parent_fk FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("parent_fk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1"), T("2")],
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
            name: "merge: conflicting merge should retain previous conflicts and constraint violations",
            intent: "A conflicting merge records its conflict and violation, and they commit when forced.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "CREATE TABLE parent (pk int PRIMARY KEY, col1 int);",
                "CREATE TABLE child (pk int PRIMARY KEY, parent_fk int, FOREIGN KEY (parent_fk) REFERENCES parent (pk));",
                "INSERT INTO parent VALUES (1, 1), (2, 1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table with data');",
                "SELECT dolt_branch('other');",
                "SELECT dolt_branch('other2');",
                "UPDATE parent SET col1 = 2 WHERE pk = 1;",
                "DELETE FROM parent WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'updating col1 to 2 and remove pk = 2');",
                "SELECT * FROM dolt_checkout('other');",
                "UPDATE parent SET col1 = 3 WHERE pk = 1;",
                "INSERT INTO child VALUES (1, 2);",
                "SELECT dolt_commit('-am', 'updating col1 to 3 and adding child of pk 2');",
                "SELECT * FROM dolt_checkout('other2');",
                "UPDATE parent SET col1 = 4 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'updating col1 to 4');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other');",
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
                    query: "SELECT * FROM parent ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM child ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("parent_fk", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_col1, base_pk, our_col1, our_pk, their_col1, their_pk FROM dolt_conflicts_parent;",
                    expected: Expected::Rows {
                        columns: &[Column("base_col1", INT4), Column("base_pk", INT4), Column("our_col1", INT4), Column("our_pk", INT4), Column("their_col1", INT4), Column("their_pk", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("2"), T("1"), T("3"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, parent_fk FROM dolt_constraint_violations_child;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("parent_fk", INT4)],
                        rows: &[
                            &[T("foreign key"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-afm', 'committing merge conflicts')) > 0 AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
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
            name: "merge: violated check constraint",
            intent: "A row the other side added that breaks our new check is a violation.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_commit('-Am', 'create table');",
                "SELECT dolt_branch('other');",
                "ALTER TABLE t ADD CHECK (col1 % 2 = 0);",
                "SELECT dolt_commit('-am', 'add check constraint');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'add row');",
                "SELECT * FROM dolt_checkout('main');",
                "SET dolt_force_transaction_commit = 1;",
                "SELECT * FROM dolt_merge('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                            &[T("public.t"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t"), T("f"), T("constraint violation")],
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
            name: "merge: ourRoot renames, theirRoot modifies",
            intent: "Rows they added to a table we renamed merge into it under its new name.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test1 VALUES (0, 1, 2);",
                "SELECT dolt_add('test1');",
                "SELECT dolt_commit('-m', 'add pk 0 to test1');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE test1 RENAME TO new_name;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'rename test1');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT fast_forward, conflicts, message FROM dolt_merge('merge_branch');",
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are matched by object id, so one side's rename merges with the other side's changes to the same table instead of reading as a drop of its old name, MERGE_PLAN §4 and Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts;",
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
                    query: "SELECT * FROM new_name;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: ourRoot renames, theirRoot modifies the schema",
            intent: "A column they dropped from a table we renamed is dropped from it under its new name.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "ALTER TABLE test1 DROP COLUMN c2;",
                "SELECT dolt_commit('-am', 'modify test1');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE test1 RENAME TO new_name;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'rename test1');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT fast_forward, conflicts, message FROM dolt_merge('merge_branch');",
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are matched by object id, so one side's rename merges with the other side's changes to the same table instead of reading as a drop of its old name, MERGE_PLAN §4 and Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts;",
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
                    query: "SELECT attname FROM pg_attribute WHERE attrelid = 'new_name'::regclass AND attnum > 0 AND NOT attisdropped ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME)],
                        rows: &[
                            &[T("pk")],
                            &[T("c1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: ourRoot modifies the data, theirRoot renames",
            intent: "Rows we added to a table they renamed stay in it under its new name.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "ALTER TABLE test1 RENAME TO new_name;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'rename test1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test1 VALUES (0, 1, 2);",
                "SELECT dolt_commit('-am', 'add pk 0 to test1');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT fast_forward, conflicts, message FROM dolt_merge('merge_branch');",
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are matched by object id, so one side's rename merges with the other side's changes to the same table instead of reading as a drop of its old name, MERGE_PLAN §4 and Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts;",
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
                    query: "SELECT * FROM new_name;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c1", INT4), Column("c2", INT4)],
                        rows: &[
                            &[T("0"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: ourRoot modifies the schema, theirRoot renames",
            intent: "A column we dropped from a table they renamed stays dropped under its new name.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "ALTER TABLE test1 RENAME TO new_name;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'rename test1');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE test1 DROP COLUMN c2;",
                "SELECT dolt_commit('-am', 'modify test1');",
                "SET dolt_allow_commit_conflicts = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT fast_forward, conflicts, message FROM dolt_merge('merge_branch');",
                    expected: Expected::Rows {
                        columns: &[Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are matched by object id, so one side's rename merges with the other side's changes to the same table instead of reading as a drop of its old name, MERGE_PLAN §4 and Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts;",
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
                    query: "SELECT attname FROM pg_attribute WHERE attrelid = 'new_name'::regclass AND attnum > 0 AND NOT attisdropped ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME)],
                        rows: &[
                            &[T("pk")],
                            &[T("c1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: dolt merge commits successful non-fast-forward merge",
            intent: "A three-way merge with --no-commit makes no commit, and without it makes the merge commit.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test1 VALUES (1, 2, 3);",
                "SELECT dolt_commit('-am', 'add (1,2,3) to test1');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO test1 VALUES (2, 3, 4);",
                "SELECT dolt_commit('-am', 'add (2,3,4) to test1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("add (1,2,3) to test1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
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
                    query: "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
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
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("merge other")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: dolt merge does not ff and not commit with --no-ff and --no-commit",
            intent: "A --no-ff --no-commit merge stages the changes, and committing them makes a merge commit with two parents.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test1 VALUES (1, 2, 3);",
                "SELECT dolt_commit('-am', 'add (1,2,3) to test1');",
                "SELECT * FROM dolt_checkout('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test1 WHERE pk = 1;",
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
                    query: "SELECT * FROM dolt_merge('main', '--no-ff', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge successful")],
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
                            &[T("t")],
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
                            &[T("public.test1"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("added tables")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-m', 'merge main')) > 0 AS committed;",
                    expected: Expected::Rows {
                        columns: &[Column("committed", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("merge main")],
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
            name: "merge: specify ---author for merge that's used for creating commit",
            intent: "A merge's --author names the merge commit's author.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test1 VALUES (1, 2, 3);",
                "SELECT dolt_commit('-am', 'add (1,2,3) to test1');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO test1 VALUES (2, 3, 4);",
                "SELECT dolt_commit('-am', 'add (2,3,4) to test1');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '--author', 'John Doe <john@doe.com>', '-m', 'merge other');",
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
                    query: "SELECT committer, email FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("committer", TEXT), Column("email", TEXT)],
                        rows: &[
                            &[T("John Doe"), T("john@doe.com")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: prints merge stats",
            intent: "A merge's changes count 2 tables, 3 rows added, 1 modified, and 1 deleted.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
                "SELECT dolt_commit('-Am', 'add table t');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (3, 3), (4, 4);",
                "DELETE FROM t WHERE pk = 1;",
                "UPDATE t SET col1 = 200 WHERE pk = 2;",
                "INSERT INTO t2 VALUES (1);",
                "SELECT dolt_commit('-Am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 5);",
                "SELECT dolt_commit('-Am', 'left');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge right into main');",
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
                    query: "SELECT count(*) AS tables, sum(rows_added) AS added, sum(rows_modified) AS modified, sum(rows_deleted) AS deleted FROM dolt_diff_stat('HEAD~1', 'HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("tables", INT8), Column("added", NUMERIC), Column("modified", NUMERIC), Column("deleted", NUMERIC)],
                        rows: &[
                            &[T("2"), T("3"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: merge with --no-commit prints correct merge stats",
            intent: "A --no-commit merge's changes count 2 tables, 3 rows added, 1 modified, and 1 deleted.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "CREATE TABLE t2 (pk int PRIMARY KEY);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
                "SELECT dolt_commit('-Am', 'add table t');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (3, 3), (4, 4);",
                "DELETE FROM t WHERE pk = 1;",
                "UPDATE t SET col1 = 200 WHERE pk = 2;",
                "INSERT INTO t2 VALUES (1);",
                "SELECT dolt_commit('-Am', 'right');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (5, 5);",
                "SELECT dolt_commit('-Am', 'left');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right', '-m', 'merge right into main', '--no-commit');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) AS tables, sum(rows_added) AS added, sum(rows_modified) AS modified, sum(rows_deleted) AS deleted FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Rows {
                        columns: &[Column("tables", INT8), Column("added", NUMERIC), Column("modified", NUMERIC), Column("deleted", NUMERIC)],
                        rows: &[
                            &[T("2"), T("3"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge: three-way merge with mergible json fails when --dont_merge_json is set",
            intent: "JSON documents each side changed with different keys merge.",
            set_up_script: &[
                "CREATE TABLE test1 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "CREATE TABLE test2 (pk int NOT NULL, c1 int, c2 int, PRIMARY KEY (pk));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'added tables');",
                "CREATE TABLE t (pk int PRIMARY KEY, j json);",
                "INSERT INTO t VALUES (1, '{}');",
                "SELECT dolt_commit('-Am', 'new table');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT * FROM dolt_checkout('b1');",
                "UPDATE t SET j = '{\"a\": 1}';",
                "SELECT dolt_commit('-Am', 'added key \"a\"');",
                "SELECT * FROM dolt_checkout('b2');",
                "UPDATE t SET j = '{\"b\": 1}';",
                "SELECT dolt_commit('-Am', 'added key \"b\"');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1');",
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
    ]);
}
