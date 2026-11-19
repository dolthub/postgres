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

//! Dolt's integration-tests/bats/sql-merge.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_merge() {
    run_scripts(&[
        ScriptTest {
            name: "sql-merge: DOLT_MERGE with no-ff displays hash.",
            intent: "A no-ff merge returns the new merge commit's hash, which is HEAD.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'commit on main');",
                "SELECT set_config('test.old_head', dolt_hashof('HEAD'), false);",
                "INSERT INTO test VALUES (679283);",
                "SELECT dolt_commit('-a', '-m', '2nd commit on main');",
                "SELECT * FROM dolt_checkout('-b', 'branch', 'HEAD~1');",
                "INSERT INTO test VALUES (380989);",
                "SELECT dolt_commit('-a', '-m', 'commit on branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.merge', hash, false) <> current_setting('test.old_head') AS found FROM dolt_merge('--no-ff', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("commit hashes never match Dolt's, so the merge commit is checked by its shape: the hash dolt_merge returns is HEAD, with two parents other than itself and Dolt's default message")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT commit_hash = current_setting('test.merge') AS head, message, parents LIKE '%, %' AS merged, position(commit_hash IN parents) = 0 AS new FROM dolt_log('--parents') LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("head", BOOL), Column("message", TEXT), Column("merged", BOOL), Column("new", BOOL)],
                        rows: &[
                            &[T("t"), T("Merge branch 'main' into branch"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE with unknown branch name throws an error",
            intent: "Merging a branch that does not exist fails.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "branch not found: feature-branch", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE works with ff",
            intent: "A fast-forward merge moves main to the branch head with its rows and a clean working set.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "UPDATE test SET pk = 1000 WHERE pk = 0;",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
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
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("this is a ff")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
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
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("1000")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test;",
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
            name: "sql-merge: DOLT_MERGE works in the session for fastforward.",
            intent: "A fast-forward merge shows its rows in the session and leaves main at the branch head.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
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
                    query: "SELECT count(*) > 0 AS found FROM test WHERE pk = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
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
                    query: "SELECT (SELECT hash FROM dolt_branches WHERE name = 'main') = (SELECT hash FROM dolt_branches WHERE name = 'feature-branch') AS found;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
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
            name: "sql-merge: DOLT_MERGE with autocommit off works in fast-forward.",
            intent: "A fast-forward merge inside a transaction block keeps its rows when the block goes on to make a branch.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('feature-branch');",
                "SELECT * FROM dolt_checkout('-b', 'new-branch');",
                "COMMIT;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE no-ff works with autocommit off.",
            intent: "A no-ff merge inside a transaction block keeps its rows once committed.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('feature-branch', '-no-ff');",
                "COMMIT;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: End to End Conflict Resolution with autocommit off.",
            intent: "Inside a transaction block, a conflicting merge resolved by deleting its conflicts commits with our row and their new one.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "CREATE TABLE test2 (pk int PRIMARY KEY, val int);",
                "INSERT INTO test2 VALUES (0, 0);",
                "BEGIN;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test2 VALUES (1, 1);",
                "UPDATE test2 SET val = 1000 WHERE pk = 0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE test2 SET val = 1001 WHERE pk = 0;",
                "SELECT dolt_commit('-a', '-m', 'update a value');",
                "SELECT * FROM dolt_merge('feature-branch', '-m', 'this is a merge');",
                "DELETE FROM dolt_conflicts_test2;",
                "SELECT dolt_commit('-a', '-m', 'remove conflicts');",
                "COMMIT;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test2 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("1001")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE works with autocommit off.",
            intent: "A three-way merge inside a transaction block keeps both sides' rows once committed.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "BEGIN;",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (5);",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit');",
                "SELECT * FROM dolt_merge('feature-branch');",
                "COMMIT;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("5")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE correctly returns head and working session variables.",
            intent: "A fast-forward merge moves the session's HEAD to the branch head.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "SELECT set_config('test.head', dolt_hashof('HEAD'), false);",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('feature-branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head') AS found;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres has no system variables named after a database, so dolt_hashof and dolt_hashof_db read the hashes of @@<db>_head and @@<db>_working, Appendix C \"per-database session variables\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Already on branch 'main'")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("the bats test runs each statement as its own CLI process, which starts on main anew, where one session that merged on main is already there, as Dolt's dolt_checkout reports")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head') AS found;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres has no system variables named after a database, so dolt_hashof and dolt_hashof_db read the hashes of @@<db>_head and @@<db>_working, Appendix C \"per-database session variables\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE correctly merges branches with differing content in same table without conflicts",
            intent: "A --no-commit merge of rows added on each side stages both, keeps the merge in progress, and commits it later.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'Insert 3');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (10000);",
                "SELECT dolt_commit('-a', '-m', 'Insert 10000');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '--no-commit');",
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
                    query: "SELECT count(*) = 2 AS found FROM test WHERE pk > 2;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("10000")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Insert 10000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("3")],
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
                            &[T("public.test"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-a', '-m', 'Finish up Merge')) > 0 AS committed;",
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
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Finish up Merge")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE works with no-ff",
            intent: "A no-ff merge makes a merge commit with its message.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'update feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-no-ff', '-m', 'this is a no-ff');",
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
                    query: "SELECT count(*) = 5 AS found FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("f")],
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
                            &[T("this is a no-ff")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE -no-ff correctly changes head and working session variables.",
            intent: "A no-ff merge moves the session's HEAD and working set.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'update feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT set_config('test.head', dolt_hashof('HEAD'), false);",
                "SELECT set_config('test.working', dolt_hashof_db('WORKING'), false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '-no-ff', '-m', 'this is a no-ff');",
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
                    query: "SELECT dolt_hashof('HEAD') <> current_setting('test.head') AS found;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres has no system variables named after a database, so dolt_hashof and dolt_hashof_db read the hashes of @@<db>_head and @@<db>_working, Appendix C \"per-database session variables\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_db('WORKING') <> current_setting('test.working') AS found;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres has no system variables named after a database, so dolt_hashof and dolt_hashof_db read the hashes of @@<db>_head and @@<db>_working, Appendix C \"per-database session variables\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE detects merge conflicts, fails to commit and leaves working set clean when dolt_allow_commit_conflicts = 0",
            intent: "A conflicting merge with dolt_allow_commit_conflicts off rolls back, leaving no merge to abort and each branch's rows as they were.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SET dolt_allow_commit_conflicts = 0;",
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'add tables');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 0, 0);",
                "SELECT dolt_commit('-a', '-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 1, 1);",
                "SELECT dolt_commit('-a', '-m', 'changed feature branch');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
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
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: There is no merge to abort", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("c1", INT8), Column("c2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'feature-branch'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("c1", INT8), Column("c2", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE detects conflicts, returns them in dolt_conflicts table",
            intent: "Inside a transaction block, a conflicting merge lists its conflicts and can be aborted, or resolved by taking their rows.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SET dolt_allow_commit_conflicts = 0;",
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'add tables');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 0, 0);",
                "SELECT dolt_commit('-a', '-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 1, 1);",
                "SELECT dolt_commit('-a', '-m', 'changed feature branch');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
                        rows: &[
                            &[T("public.one_pk"), T("1")],
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
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
                    query: "INSERT INTO one_pk (pk1, c1, c2) SELECT their_pk1, their_c1, their_c2 FROM dolt_conflicts_one_pk WHERE their_pk1 IS NOT NULL ON CONFLICT (pk1) DO UPDATE SET c1 = EXCLUDED.c1, c2 = EXCLUDED.c2;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM one_pk WHERE pk1 IN (SELECT base_pk1 FROM dolt_conflicts_one_pk WHERE their_pk1 IS NULL);",
                    expected: Expected::Tag("DELETE 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_one_pk;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-a', '-m', 'Finish Resolving')) > 0 AS committed;",
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("c1", INT8), Column("c2", INT8)],
                        rows: &[
                            &[T("0"), T("1"), T("1")],
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
            name: "sql-merge: DOLT_MERGE(--abort) clears session state and allows additional edits",
            intent: "Aborting a merge inside a transaction block lets the block go on writing, and its rows stay once committed.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "BEGIN;",
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'add tables');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 0, 0);",
                "SELECT dolt_commit('-a', '-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 1, 1);",
                "SELECT dolt_commit('-a', '-m', 'changed feature branch');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('feature-branch');",
                "SELECT * FROM dolt_merge('--abort');",
                "INSERT INTO one_pk VALUES (9, 9, 9);",
                "COMMIT;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.one_pk"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_pk1, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'one_pk');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk1", INT8), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("9"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Dolt's own columns of its per-table system tables and dolt_diff() carry the dolt_ prefix, by the owner's decision 2026-10-26 (Appendix C)")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM one_pk ORDER BY pk1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk1", INT8), Column("c1", INT8), Column("c2", INT8)],
                        rows: &[
                            &[T("0"), T("0"), T("0")],
                            &[T("9"), T("9"), T("9")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE(--abort) clears index state",
            intent: "Aborting a merge inside a transaction block leaves a clean working set.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "BEGIN;",
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'add tables');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 0, 0);",
                "SELECT dolt_commit('-a', '-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 1, 1);",
                "SELECT dolt_commit('-a', '-m', 'changed feature branch');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('feature-branch');",
                "SELECT * FROM dolt_merge('--abort');",
                "COMMIT;",
            ],
            assertions: &[
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
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE can commit unresolved conflicts with dolt_allow_commit_conflicts set",
            intent: "With dolt_allow_commit_conflicts on, a conflicting merge commits its conflicts, and merging again is refused until the merge is committed.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SET dolt_allow_commit_conflicts = on;",
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'add tables');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 0, 0);",
                "SELECT dolt_commit('-a', '-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 1, 1);",
                "SELECT dolt_commit('-a', '-m', 'changed feature branch');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('feature-branch');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
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
                    query: "SELECT * FROM dolt_merge('feature-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merging is not possible because you have not committed an active merge", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE during an active merge throws an error",
            intent: "Merging while a --no-commit merge is in progress fails.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'Insert 3');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (500000);",
                "SELECT dolt_commit('-a', '-m', 'Insert 500000');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '--no-commit');",
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
                    query: "SELECT * FROM dolt_merge('feature-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "merging is not possible because you have not committed an active merge", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: CALL DOLT_MERGE works with ff and squash",
            intent: "A fast-forward squash merge stages the rows without moving HEAD, and committing it leaves a clean working set.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'this is a ff');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '--squash');",
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
                    query: "SELECT count(*) > 0 AS found FROM test WHERE pk = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
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
                            &[T("Step 1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("2")],
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
                            &[T("public.test"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-a', '-m', 'hi')) > 0 AS committed;",
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
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
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
            name: "sql-merge: DOLT_MERGE with no-ff and squash works.",
            intent: "A squash merge with --no-commit stages the changes, and committing them makes one commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'Insert 3');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (500000);",
                "SELECT dolt_commit('-a', '-m', 'Insert 500000');",
                "SELECT * FROM dolt_merge('feature-branch', '--squash', '--no-commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-a', '-m', 'Finish up Merge')) > 0 AS committed;",
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
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Finish up Merge")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE with a long series of changing operations works.",
            intent: "Rows inserted, deleted, and rekeyed on each side merge to the net result of both.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "INSERT INTO test VALUES (4);",
                "INSERT INTO test VALUES (21232);",
                "DELETE FROM test WHERE pk = 4;",
                "UPDATE test SET pk = 21 WHERE pk = 21232;",
                "SELECT dolt_commit('-a', '-m', 'Insert 3');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (500000);",
                "INSERT INTO test VALUES (500001);",
                "DELETE FROM test WHERE pk = 500001;",
                "UPDATE test SET pk = 60 WHERE pk = 500000;",
                "SELECT dolt_commit('-a', '-m', 'Insert 60');",
                "SELECT * FROM dolt_merge('feature-branch', '--no-commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-a', '-m', 'Finish up Merge')) > 0 AS committed;",
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
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Finish up Merge")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("21")],
                            &[T("60")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM test;",
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
        ScriptTest {
            name: "sql-merge: DOLT_MERGE with conflicts renders the dolt_conflicts table",
            intent: "Inside a transaction block, a conflicting merge reports its conflict with a warning, and the conflicts table shows it.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "BEGIN;",
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'add tables');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 0, 0);",
                "SELECT dolt_commit('-a', '-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 1, 1);",
                "SELECT dolt_commit('-a', '-m', 'changed feature branch');",
                "SELECT * FROM dolt_checkout('main');",
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
                    dolt: Some(Dolt::MySqlOnly("SHOW WARNINGS is MySQL's; Postgres sends each warning with the query that raised it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts WHERE num_conflicts > 0;",
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
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE with conflicts is queryable when autocommit is on",
            intent: "A conflicting merge with autocommit on reports its conflict with a warning, and the conflicts table shows it.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "CREATE TABLE one_pk (pk1 bigint NOT NULL, c1 bigint, c2 bigint, PRIMARY KEY (pk1));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'add tables');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 0, 0);",
                "SELECT dolt_commit('-a', '-m', 'changed main');",
                "SELECT * FROM dolt_checkout('feature-branch');",
                "INSERT INTO one_pk (pk1, c1, c2) VALUES (0, 1, 1);",
                "SELECT dolt_commit('-a', '-m', 'changed feature branch');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    dolt: Some(Dolt::Differs("Dolt skips it (This needs to work): its CLI test expects the conflicts to stay, where Dolt's sql-server, as asserted, warns of them and then rolls the merge back as the autocommit transaction commits. SHOW WARNINGS is MySQL's; Postgres sends each warning with the query that raised it; the message also names the setting as Postgres sets it, with no @@")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts WHERE num_conflicts > 0;",
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
            name: "sql-merge: up-to-date branch does not error",
            intent: "Merging a branch that main is already ahead of changes nothing.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'commit all changes');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'a commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("cannot fast forward from a to b. a is ahead of b already")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: up-to-date branch does not error on CALL",
            intent: "Merging a branch that main is already ahead of changes nothing.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'commit all changes');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'a commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("0"), T("cannot fast forward from a to b. a is ahead of b already")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: adding and dropping primary keys any number of times not produce schema merge conflicts",
            intent: "A primary key added and dropped repeatedly on one side merges with the same key added on the other.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'commit all changes');",
                "CREATE TABLE test_null (i int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial');",
                "SELECT * FROM dolt_checkout('-b', 'b1');",
                "ALTER TABLE test_null ADD PRIMARY KEY (i);",
                "ALTER TABLE test_null DROP CONSTRAINT test_null_pkey;",
                "ALTER TABLE test_null ADD PRIMARY KEY (i);",
                "ALTER TABLE test_null DROP CONSTRAINT test_null_pkey;",
                "ALTER TABLE test_null ADD PRIMARY KEY (i);",
                "SELECT dolt_commit('-am', 'b1 primary key changes');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE test_null ADD PRIMARY KEY (i);",
                "SELECT dolt_commit('-am', 'main primary key changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1', '-m', 'merge');",
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
            name: "sql-merge: identical schema changes with data changes merges correctly",
            intent: "The same column added on two branches, each with its own row, merges one after the other.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial commit');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT * FROM dolt_checkout('b1');",
                "ALTER TABLE t ADD COLUMN j int;",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'changes to b1');",
                "SELECT * FROM dolt_checkout('b2');",
                "ALTER TABLE t ADD COLUMN j int;",
                "INSERT INTO t VALUES (2, 2);",
                "SELECT dolt_commit('-am', 'changes to b2');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1', '-m', 'merge b1');",
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
                    query: "SELECT * FROM dolt_merge('b2', '-m', 'merge b2');",
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
            name: "sql-merge: non-conflicting data and constraint changes are preserved",
            intent: "A row added on one side and a check added on the other both survive the merge.",
            set_up_script: &[
                "CREATE TABLE t (i int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial commit');",
                "SELECT dolt_branch('other');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO t VALUES (1);",
                "SELECT dolt_commit('-am', 'changes to other');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD CHECK (i < 10);",
                "SELECT dolt_commit('-am', 'changes to main');",
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
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname::text, pg_get_constraintdef(oid) AS definition FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'c' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", TEXT), Column("definition", TEXT)],
                        rows: &[
                            &[T("t_i_check"), T("CHECK ((i < 10))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so pg_get_constraintdef shows the checks")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: non-overlapping check constraints merge successfully",
            intent: "Checks added on each side both survive the merge.",
            set_up_script: &[
                "CREATE TABLE t (i int, j int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "ALTER TABLE t ADD CONSTRAINT c0 CHECK (i > 0);",
                "SELECT dolt_commit('-am', 'changes to other');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD CONSTRAINT c1 CHECK (j < 0);",
                "SELECT dolt_commit('-am', 'changes to main');",
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
                    query: "SELECT conname::text, pg_get_constraintdef(oid) AS definition FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'c' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", TEXT), Column("definition", TEXT)],
                        rows: &[
                            &[T("c0"), T("CHECK ((i > 0))")],
                            &[T("c1"), T("CHECK ((j < 0))")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so pg_get_constraintdef shows the checks")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: different check constraints on same column throw conflict",
            intent: "Different checks added on each side are a schema conflict that --ours resolves to our check.",
            set_up_script: &[
                "CREATE TABLE t (i int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'other');",
                "ALTER TABLE t ADD CONSTRAINT c0 CHECK (i > 0);",
                "SELECT dolt_commit('-am', 'changes to other');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t ADD CONSTRAINT c1 CHECK (i < 0);",
                "SELECT dolt_commit('-am', 'changes to main');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
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
                            &[T("public.t"), T("f"), T("schema conflict")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts;",
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
                    query: "SELECT conname::text, pg_get_constraintdef(oid) AS definition FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'c' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", TEXT), Column("definition", TEXT)],
                        rows: &[
                            &[T("c1"), T("CHECK ((i < 0))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so pg_get_constraintdef shows the checks")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: dropping constraint on both branches merges successfully",
            intent: "A check dropped on both sides is gone after the merge.",
            set_up_script: &[
                "CREATE TABLE t (i int);",
                "SELECT dolt_add('.');",
                "ALTER TABLE t ADD CONSTRAINT c CHECK (i > 0);",
                "SELECT dolt_commit('-am', 'initial commit');",
                "SELECT dolt_branch('other');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t DROP CONSTRAINT c;",
                "SELECT dolt_commit('-am', 'changes to other');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP CONSTRAINT c;",
                "SELECT dolt_commit('-am', 'changes to main');",
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
                    query: "SELECT conname::text, pg_get_constraintdef(oid) AS definition FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'c' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", TEXT), Column("definition", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so pg_get_constraintdef shows the checks")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: dropping constraint in one branch and modifying same in other results in conflict",
            intent: "A check dropped on one side and changed on the other is a schema conflict that --ours resolves to our check.",
            set_up_script: &[
                "CREATE TABLE t (i int);",
                "SELECT dolt_add('.');",
                "ALTER TABLE t ADD CONSTRAINT c CHECK (i > 0);",
                "SELECT dolt_commit('-am', 'initial commit');",
                "SELECT dolt_branch('other');",
                "SELECT * FROM dolt_checkout('other');",
                "ALTER TABLE t DROP CONSTRAINT c;",
                "SELECT dolt_commit('-am', 'changes to other');",
                "SELECT * FROM dolt_checkout('main');",
                "ALTER TABLE t DROP CONSTRAINT c;",
                "ALTER TABLE t ADD CONSTRAINT c CHECK (i < 10);",
                "SELECT dolt_commit('-am', 'changes to main');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('other', '-m', 'merge other');",
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
                            &[T("public.t"), T("f"), T("schema conflict")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts;",
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
                    query: "SELECT conname::text, pg_get_constraintdef(oid) AS definition FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'c' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", TEXT), Column("definition", TEXT)],
                        rows: &[
                            &[T("c"), T("CHECK ((i < 10))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so pg_get_constraintdef shows the checks")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: merging with not null and check constraints preserves both constraints",
            intent: "A check from one branch and NOT NULL from another both survive their merges.",
            set_up_script: &[
                "CREATE TABLE t (i int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial commit');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT * FROM dolt_checkout('b1');",
                "ALTER TABLE t ADD CHECK (i > 0);",
                "SELECT dolt_commit('-am', 'changes to b1');",
                "SELECT * FROM dolt_checkout('b2');",
                "ALTER TABLE t ALTER COLUMN i TYPE int, ALTER COLUMN i SET NOT NULL, ALTER COLUMN i DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'changes to b2');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b1', '-m', 'merge b1');",
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
                    query: "SELECT * FROM dolt_merge('b2', '-m', 'merge b2');",
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
                    query: "SELECT attnotnull FROM pg_attribute WHERE attrelid = 't'::regclass AND attname = 'i';",
                    expected: Expected::Rows {
                        columns: &[Column("attnotnull", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so pg_get_constraintdef shows the checks")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname::text, pg_get_constraintdef(oid) AS definition FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'c' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", TEXT), Column("definition", TEXT)],
                        rows: &[
                            &[T("t_i_check"), T("CHECK ((i > 0))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so pg_get_constraintdef shows the checks")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: check constraint with name collision",
            intent: "Checks of the same name with different expressions on each side are a schema conflict that --ours resolves to our check.",
            set_up_script: &[
                "CREATE TABLE t (i int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial commit');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT * FROM dolt_checkout('b1');",
                "ALTER TABLE t ADD CONSTRAINT c CHECK (i > 0);",
                "SELECT dolt_commit('-am', 'changes to b1');",
                "SELECT * FROM dolt_checkout('b2');",
                "ALTER TABLE t ADD CONSTRAINT c CHECK (i < 10);",
                "SELECT dolt_commit('-am', 'changes to b2');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('b1', '-m', 'merge b1', '--commit');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b2', '-m', 'merge b2');",
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
                            &[T("public.t"), T("f"), T("schema conflict")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts;",
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
                    query: "SELECT conname::text, pg_get_constraintdef(oid) AS definition FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'c' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", TEXT), Column("definition", TEXT)],
                        rows: &[
                            &[T("c"), T("CHECK ((i > 0))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so pg_get_constraintdef shows the checks")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: check constraint for deleted column in another table",
            intent: "A check on a column the other side dropped is a schema conflict that --ours resolves to our check.",
            set_up_script: &[
                "CREATE TABLE t (i int PRIMARY KEY, j int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'initial commit');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT * FROM dolt_checkout('b1');",
                "ALTER TABLE t ADD CONSTRAINT c CHECK (j > 0);",
                "SELECT dolt_commit('-am', 'changes to b1');",
                "SELECT * FROM dolt_checkout('b2');",
                "ALTER TABLE t DROP COLUMN j;",
                "SELECT dolt_commit('-am', 'changes to b2');",
                "SELECT * FROM dolt_checkout('main');",
                "SELECT * FROM dolt_merge('b1', '-m', 'merge b1');",
                "SET dolt_force_transaction_commit = 1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('b2', '-m', 'merge b2');",
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
                            &[T("public.t"), T("f"), T("schema conflict")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_schema_conflicts;",
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
                    query: "SELECT conname::text, pg_get_constraintdef(oid) AS definition FROM pg_constraint WHERE conrelid = 't'::regclass AND contype = 'c' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", TEXT), Column("definition", TEXT)],
                        rows: &[
                            &[T("c"), T("CHECK ((j > 0))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's, so pg_get_constraintdef shows the checks")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-merge: DOLT_MERGE with author flag specified",
            intent: "A merge's --author names the merge commit's author.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_commit('-a', '-m', 'add 3 from other');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (4);",
                "SELECT dolt_commit('-a', '-m', 'add 4 from main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch', '--author', 'John Doe <john@doe.com>');",
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
            name: "sql-merge: ff merge doesn't stomp working changes",
            intent: "A fast-forward merge keeps another table's uncommitted changes.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "CREATE TABLE test2 (pk int);",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test VALUES (9);",
                "SELECT dolt_commit('-a', '-m', 'modify test');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test2 VALUES (8);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("new table")],
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
                    query: "SELECT count(*) FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
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
                            &[T("public.test2"), T("f"), T("new table")],
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
            name: "sql-merge: no-ff merge doesn't stomp working changes and doesn't fast forward",
            intent: "A no-ff merge keeps another table's uncommitted changes and makes a merge commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test VALUES (9);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'modify test');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE t2 (pk int);",
                "INSERT INTO t2 VALUES (0);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t2"), T("f"), T("new table")],
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
                            &[T("public.t2"), T("f"), T("new table")],
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
            name: "sql-merge: squash merge",
            intent: "A squash merge stages the branch's changes beside another table's, and the squashed commit is not in the log.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test VALUES (3);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'add pk 3 to test');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (4);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'add pk 4 to test1');",
                "CREATE TABLE t2 (pk int);",
                "INSERT INTO t2 VALUES (9);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t2"), T("f"), T("new table")],
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
                            &[T("public.t2"), T("f"), T("new table")],
                            &[T("public.test"), T("t"), T("modified")],
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
                    query: "SELECT count(*) FROM dolt_log WHERE message = 'add pk 4 to test1';",
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
                    query: "SELECT count(*) FROM dolt_log WHERE message = 'add pk 3 to test';",
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
            name: "sql-merge: --abort restores working changes",
            intent: "Aborting a merge restores another table's uncommitted changes, with no merge in progress.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT dolt_branch('other');",
                "INSERT INTO test VALUES (9), (8);",
                "SELECT dolt_commit('-am', 'added rows to test on main');",
                "SELECT * FROM dolt_checkout('other');",
                "INSERT INTO test VALUES (7), (6);",
                "SELECT dolt_commit('-am', 'added rows to test on other');",
                "SELECT * FROM dolt_checkout('main');",
                "CREATE TABLE t2 (pk int);",
                "INSERT INTO t2 VALUES (5);",
                "SELECT * FROM dolt_merge('other', '--no-commit');",
            ],
            assertions: &[
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
                    query: "SELECT * FROM dolt_merge_status;",
                    expected: Expected::Rows {
                        columns: &[Column("is_merging", BOOL), Column("source", TEXT), Column("source_commit", TEXT), Column("target", TEXT), Column("unmerged_tables", TEXT)],
                        rows: &[
                            &[T("f"), Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
            name: "sql-merge: 3way merge doesn't stomp working changes",
            intent: "A --no-commit three-way merge stages its changes beside another table's, and committing it keeps both sides' commits in the log.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (0), (1), (2);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'merge_branch');",
                "INSERT INTO test VALUES (9);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'add pk 9 to test1');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO test VALUES (8);",
                "SELECT dolt_add('test');",
                "SELECT dolt_commit('-m', 'add pk 8 to test1');",
                "CREATE TABLE t2 (pk int);",
                "INSERT INTO t2 VALUES (7);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t2"), T("f"), T("new table")],
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
                            &[T("public.t2"), T("f"), T("new table")],
                            &[T("public.test"), T("t"), T("modified")],
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
                    query: "SELECT count(*) FROM dolt_log WHERE message IN ('add pk 9 to test1', 'add pk 8 to test1');",
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
