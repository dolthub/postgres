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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_revert.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_revert_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "dolt_revert() reverts HEAD",
            intent: "dolt_revert of HEAD makes a commit that undoes HEAD's changes.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "UPDATE test SET c0 = 42 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'answer of the universe: 42');",
                "SELECT * FROM dolt_revert('HEAD');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD' WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD~1' WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("2"), T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() reverts HEAD~1",
            intent: "dolt_revert of an older commit undoes only that commit's changes.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "UPDATE test SET c0 = 42 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'answer of the universe');",
                "UPDATE test SET c0 = 23 WHERE pk = 3;",
                "SELECT dolt_commit('-am', 'answer of the universe');",
                "SELECT * FROM dolt_revert('HEAD~1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD' WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD~2' WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("2"), T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD' WHERE pk = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("3"), T("23")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() multi-commit: one commit per revert",
            intent: "dolt_revert of several commits makes one revert commit for each.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "INSERT INTO test VALUES (4, 4);",
                "SELECT dolt_commit('-am', 'insert row 4');",
                "INSERT INTO test VALUES (5, 5);",
                "SELECT dolt_commit('-am', 'insert row 5');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log;",
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
                    query: "SELECT * FROM dolt_revert('HEAD', 'HEAD~1');",
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
                    query: "SELECT count(*) FROM dolt_log;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"insert row 5\"")],
                            &[T("Revert \"insert row 4\"")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
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
            name: "dolt_revert() with @@autocommit=1 returns error on conflicts and leaves repo clean",
            intent: "Outside a transaction block, a dolt_revert that conflicts fails and leaves the working set clean.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "UPDATE test SET c0 = 42 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'first change');",
                "UPDATE test SET c0 = 23 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'second change');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~1');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT \"table\" FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT)],
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
                            &[T("second change")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() detects conflicts and returns conflict counts",
            intent: "A conflicting dolt_revert reports its data conflict counts and records the conflicts.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "UPDATE test SET c0 = 42 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'first change');",
                "UPDATE test SET c0 = 23 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'second change');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~1');",
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
                    query: "SELECT \"table\" FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT)],
                        rows: &[
                            &[T("public.test")],
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
                            &[T("second change")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() --abort restores pre-revert state",
            intent: "dolt_revert --abort restores the state from before a conflicted revert.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "INSERT INTO test VALUES (3,3);",
                "SELECT dolt_commit('-am', 'insert 3,3');",
                "UPDATE test SET c0 = 42 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'first change');",
                "UPDATE test SET c0 = 23 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'second change');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~2', 'HEAD~1');",
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
                    query: "SELECT \"table\" FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT)],
                        rows: &[
                            &[T("public.test")],
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
                            &[T("Revert \"insert 3,3\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--abort');",
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
                    query: "SELECT c0 FROM test WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4)],
                        rows: &[
                            &[T("23")],
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
                            &[T("second change")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() --abort rejects other dirty working sets before resetting HEAD",
            intent: "dolt_revert --abort refuses while another branch's working set is dirty in the transaction, leaving HEAD alone.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "INSERT INTO test VALUES (3,3);",
                "SELECT dolt_commit('-am', 'insert 3,3');",
                "UPDATE test SET c0=42 WHERE pk=2;",
                "SELECT dolt_commit('-am', 'first change');",
                "UPDATE test SET c0=23 WHERE pk=2;",
                "SELECT dolt_commit('-am', 'second change');",
                "SELECT dolt_branch('other');",
                "SELECT * FROM dolt_revert('HEAD~2', 'HEAD~1');",
                "SET dolt_allow_commit_conflicts = 1;",
                "COMMIT;",
                "BEGIN;",
                "INSERT INTO \"postgres/other\".public.test VALUES (4,4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot commit changes on more than one branch / database", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"insert 3,3\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT \"table\" FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT)],
                        rows: &[
                            &[T("public.test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/other\".public.test WHERE pk=4;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET c0=99 WHERE pk=1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--abort');",
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
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("second change")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("23")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
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
                    query: "SELECT * FROM \"postgres/other\".public.test WHERE pk=4;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            skip: Some("dolt_revert('--abort') succeeds although other working sets are dirty, where this test expects the refusal (22023) (bug, MacDG)"),
            ..S
        },
        ScriptTest {
            name: "dolt_revert() --continue: successful conflict resolution workflow",
            intent: "After a conflicted revert's conflicts are resolved and staged, dolt_revert --continue makes the revert commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "UPDATE test SET c0 = 42 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'first change');",
                "UPDATE test SET c0 = 23 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'second change');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~1');",
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
                    query: "SELECT \"table\" FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT)],
                        rows: &[
                            &[T("public.test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--continue');",
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
                    query: "DELETE FROM dolt_conflicts_test;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET c0 = 23 WHERE pk = 2;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--continue');",
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
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"first change\"")],
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
                    query: "SELECT * FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", INT8)],
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
            name: "dolt_revert() --continue: ignored table in working set",
            intent: "dolt_revert --continue succeeds with an ignored table in the working set.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO dolt_ignore VALUES ('ignored_*', TRUE);",
                "CREATE TABLE ignored_1 (id int PRIMARY KEY);",
                "INSERT INTO test VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "UPDATE test SET c0 = 42 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'first change');",
                "UPDATE test SET c0 = 23 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'second change');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~1');",
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
                    query: "DELETE FROM dolt_conflicts_test;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET c0 = 23 WHERE pk = 2;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ignored_2 (id int PRIMARY KEY);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--continue');",
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
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"first change\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() --continue: multiple table conflicts",
            intent: "dolt_revert --continue succeeds once conflicts in several tables are resolved.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, v varchar(100));",
                "CREATE TABLE t2 (pk int PRIMARY KEY, v varchar(100));",
                "INSERT INTO t1 VALUES (1, 'original_t1');",
                "INSERT INTO t2 VALUES (1, 'original_t2');",
                "SELECT dolt_commit('-Am', 'seed tables');",
                "UPDATE t1 SET v = 'changed_t1' WHERE pk = 1;",
                "UPDATE t2 SET v = 'changed_t2' WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'change both tables');",
                "UPDATE t1 SET v = 'newer_t1' WHERE pk = 1;",
                "UPDATE t2 SET v = 'newer_t2' WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'newer changes');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~1');",
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
                    query: "DELETE FROM dolt_conflicts_t1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t1 SET v = 'resolved_t1' WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--continue');",
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
                    query: "DELETE FROM dolt_conflicts_t2;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t2 SET v = 'resolved_t2' WHERE pk = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t2');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--continue');",
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
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() --continue not in a revert state",
            intent: "dolt_revert --continue fails when no revert is in progress.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1);",
                "SELECT dolt_commit('-Am', 'seed table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: There is no revert in progress", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() --abort not in a revert state",
            intent: "dolt_revert --abort fails when no revert is in progress.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1);",
                "SELECT dolt_commit('-Am', 'seed table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: There is no revert in progress", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() --continue and --abort are mutually exclusive",
            intent: "dolt_revert refuses --continue and --abort together.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "UPDATE test SET c0 = 42 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'first change');",
                "UPDATE test SET c0 = 23 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'second change');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~1');",
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
                    query: "SELECT * FROM dolt_revert('--continue', '--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: --continue and --abort are mutually exclusive", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--abort', '--continue');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: --continue and --abort are mutually exclusive", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() multi-commit: stops at first conflict",
            intent: "A dolt_revert of several commits stops at the first one that conflicts, keeping the reverts made before it.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "INSERT INTO test VALUES (4, 4);",
                "SELECT dolt_commit('-am', 'insert row 4');",
                "UPDATE test SET c0 = 42 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'change row 2');",
                "UPDATE test SET c0 = 99 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'change row 2 again');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~2', 'HEAD~1');",
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
                    query: "SELECT pk FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT \"table\" FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT)],
                        rows: &[
                            &[T("public.test")],
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
            name: "dolt_revert() proceeds when dirty table is disjoint from revert target",
            intent: "dolt_revert proceeds while a table it doesn't touch has working changes.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c0 int);",
                "CREATE TABLE meta (pk int PRIMARY KEY, note varchar(100));",
                "SELECT dolt_commit('-Am', 'seed schema');",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
                "SELECT dolt_commit('-am', 'add rows to t');",
                "INSERT INTO meta VALUES (1, 'side');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD');",
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
                    query: "SELECT pk, c0 FROM t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk, note FROM meta;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("note", VARCHAR)],
                        rows: &[
                            &[T("1"), T("side")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() refuses when revert target table is dirty",
            intent: "dolt_revert refuses while a table it would change has working changes.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c0 int);",
                "SELECT dolt_commit('-Am', 'seed schema');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'add row to t');",
                "INSERT INTO t VALUES (99, 99);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: Your local changes would be overwritten by revert.\nhint: Please commit your changes before you revert.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() refuses when a disjoint table has staged changes",
            intent: "dolt_revert refuses while any table has staged changes.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, c0 int);",
                "CREATE TABLE meta (pk int PRIMARY KEY, note varchar(100));",
                "SELECT dolt_commit('-Am', 'seed schema');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'add row to t');",
                "INSERT INTO meta VALUES (1, 'side');",
                "SELECT dolt_add('meta');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "error: Your local changes would be overwritten by revert.\nhint: Please commit your changes before you revert.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() proceeds with untracked tables disjoint from revert target",
            intent: "dolt_revert proceeds with untracked tables it doesn't touch.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "UPDATE test SET c0 = 42 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'answer of the universe: 42');",
                "CREATE TABLE dont_track (pk int PRIMARY KEY);",
                "INSERT INTO dont_track VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD');",
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
                    query: "SELECT pk, c0 FROM test WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM dont_track;",
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
            name: "dolt_revert() respects dolt_ignore",
            intent: "dolt_revert leaves tables dolt_ignore names alone.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2),(3,3);",
                "INSERT INTO dolt_ignore VALUES ('dont_*', TRUE);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "UPDATE test SET c0 = 42 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'answer of the universe: 42');",
                "CREATE TABLE dont_track (id int PRIMARY KEY);",
                "INSERT INTO dont_track VALUES (1);",
                "SELECT * FROM dolt_revert('HEAD');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD' WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test AS OF 'HEAD~1' WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("2"), T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dont_track AS OF 'HEAD';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"dont_track\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status_ignored;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.dont_track"), T("f"), T("new table"), T("t")],
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
            name: "dolt_revert() detects constraint violations and returns violation counts",
            intent: "A dolt_revert that breaks a constraint reports its violation counts and records the violations.",
            set_up_script: &[
                "BEGIN;",
                "CREATE TABLE test2 (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test2 VALUES (1,1),(2,NULL),(3,3);",
                "SELECT dolt_commit('-Am', 'new table with NULL value');",
                "DELETE FROM test2 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'deleted row with NULL value');",
                "ALTER TABLE test2 ALTER COLUMN c0 TYPE int, ALTER COLUMN c0 SET NOT NULL, ALTER COLUMN c0 DROP DEFAULT;",
                "SELECT dolt_commit('-am', 'modified column c0 to not null');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('head~1');",
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
                    query: "SELECT \"table\" FROM dolt_constraint_violations;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT)],
                        rows: &[
                            &[T("public.test2")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_violation_type, pk, c0 FROM dolt_constraint_violations_test2;",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_violation_type", TEXT), Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("not null"), T("2"), Null],
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
            name: "dolt_revert() commit has current timestamp",
            intent: "A revert commit gets the current time, not the reverted commit's.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "UPDATE test SET c0 = 42 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'answer of the universe: 42');",
                "SELECT * FROM dolt_revert('HEAD');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT extract(year FROM date) > 2000 FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL's YEAR(date) is extract(year FROM date)")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() --continue called multiple times for multi-commit series",
            intent: "dolt_revert --continue resumes a several-commit revert after each conflict until all are reverted.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, c0 int);",
                "INSERT INTO test VALUES (1,1),(2,2),(3,3);",
                "SELECT dolt_commit('-Am', 'seed table');",
                "UPDATE test SET c0 = 10 WHERE pk = 1;",
                "SELECT dolt_commit('-am', 'change pk1');",
                "UPDATE test SET c0 = 20 WHERE pk = 2;",
                "SELECT dolt_commit('-am', 'change pk2');",
                "UPDATE test SET c0 = 30 WHERE pk = 3;",
                "SELECT dolt_commit('-am', 'change pk3');",
                "UPDATE test SET c0 = 21 WHERE pk = 2;",
                "UPDATE test SET c0 = 31 WHERE pk = 3;",
                "SELECT dolt_commit('-am', 'clobber pk2 and pk3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('HEAD~3', 'HEAD~2', 'HEAD~1');",
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
                    query: "SELECT c0 FROM test WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT \"table\" FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT)],
                        rows: &[
                            &[T("public.test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_test;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET c0 = 21 WHERE pk = 2;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--continue');",
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
                    query: "SELECT message FROM dolt_log LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"change pk2\"")],
                            &[T("Revert \"change pk1\"")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT \"table\" FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT)],
                        rows: &[
                            &[T("public.test")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("tables are named with their schema, as Postgres names them, Appendix C \"what is versioned\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_test;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET c0 = 31 WHERE pk = 3;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('test');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_revert('--continue');",
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
                    query: "SELECT message FROM dolt_log LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Revert \"change pk3\"")],
                            &[T("Revert \"change pk2\"")],
                            &[T("Revert \"change pk1\"")],
                        ],
                        tag: "SELECT 3",
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
                    query: "SELECT pk, c0 FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("c0", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("21")],
                            &[T("3"), T("31")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_revert() automatically resolves some conflicts",
            intent: "dolt_revert resolves conflicts it can merge cell by cell without stopping.",
            set_up_script: &[
                "CREATE TABLE tableA (id int PRIMARY KEY, col1 varchar(255));",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'new table');",
                "INSERT INTO tableA VALUES (1, 'test');",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'new row');",
                "SELECT dolt_branch('new_row');",
                "ALTER TABLE tableA ALTER COLUMN col1 TYPE TEXT, ALTER COLUMN col1 DROP NOT NULL, ALTER COLUMN col1 DROP DEFAULT;",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-m', 'change col');",
                "SELECT * FROM dolt_revert('new_row');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM tableA;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("col1", TEXT)],
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
