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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_multi_branch_commit_queries.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_multi_branch_commit_tests() {
    run_scripts(&[
        ScriptTest {
            name: "multi branch commit defaults off but SQL working set commits still work",
            intent: "With multi-branch commits off, dolt_commit refuses a transaction that wrote two branches, while its plain COMMIT keeps both working sets.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "BEGIN;",
                "INSERT INTO t VALUES (1);",
                "INSERT INTO \"postgres/b1\".public.t VALUES (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT current_setting('dolt_multi_branch_commit'), current_setting('dolt_multi_branch_commit');",
                    expected: Expected::Rows {
                        columns: &[Column("current_setting", TEXT), Column("current_setting", TEXT)],
                        rows: &[
                            &[T("off"), T("off")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'rejected');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot commit changes on more than one branch / database", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
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
                    query: "SELECT * FROM \"postgres/b1\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
            name: "transaction commits reject multiple branches until enabled",
            intent: "dolt_transaction_commit refuses a transaction that wrote two branches until dolt_multi_branch_commit is on, then commits both with the message.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "BEGIN;",
                "SET dolt_transaction_commit = 1;",
                "SET dolt_transaction_commit_message = 'multi transaction';",
                "INSERT INTO t VALUES (1);",
                "INSERT INTO \"postgres/b1\".public.t VALUES (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot commit changes on more than one branch / database", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
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
                    query: "SELECT * FROM t AS OF 'HEAD';",
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
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("2")],
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
                            &[T("multi transaction")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/b1\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("multi transaction")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "multi branch dolt_commit returns one hash per branch and can be disabled again",
            intent: "With dolt_multi_branch_commit on, dolt_commit commits every written branch and returns a hash for each, and turning it off restores the refusal.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "BEGIN;",
                "SET dolt_multi_branch_commit = 1;",
                "INSERT INTO t VALUES (1);",
                "INSERT INTO \"postgres/b1\".public.t VALUES (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'multi');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'HEAD';",
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
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 0;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (4);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'rejected');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot commit changes on more than one branch / database", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'explicit');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "automatic multi branch commit stages new tables with a clean selected branch",
            intent: "An automatic multi-branch commit stages new tables on every written branch, even when the selected branch is clean.",
            set_up_script: &[
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "CREATE TABLE \"postgres/b1\".public.created (pk int PRIMARY KEY);",
                "CREATE TABLE \"postgres/b2\".public.created (pk int PRIMARY KEY);",
                "BEGIN;",
                "SET dolt_multi_branch_commit = 1;",
                "SET dolt_transaction_commit = 1;",
                "INSERT INTO \"postgres/b1\".public.created VALUES (1);",
                "INSERT INTO \"postgres/b2\".public.created VALUES (2);",
            ],
            assertions: &[
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
                    query: "SELECT * FROM \"postgres/b1\".public.created AS OF 'HEAD';",
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
                    query: "SELECT * FROM \"postgres/b2\".public.created AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/b1\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Transaction commit")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT CURRENT_SCHEMA(), active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("current_schema", NAME), Column("active_branch", TEXT)],
                        rows: &[
                            &[T("postgres"), T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a table created through a revision-qualified name (\"postgres/b1\".public.created) and a name read AS OF a revision relative to its branch"),
            ..S
        },
        ScriptTest {
            name: "enabled multi branch commits still reject multiple physical databases",
            intent: "Multi-branch commits still refuse a transaction that wrote two different databases.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am','setup');",
                "CREATE database other;",
                "BEGIN;",
                "SET dolt_multi_branch_commit = 1;",
                "INSERT INTO t VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO other.public.t VALUES (2);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cross-database references are not implemented: \"other.public.t\"", position: 13, ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses a write to another database, so no transaction changes two databases' working states, where Dolt refuses their commit with \"Cannot atomically commit changes to more than one database\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
fn test_multi_branch_automatic_commit_transactions() {
    run_scripts(&[
        ScriptTest {
            name: "three clients merge partially overlapping branch sets",
            intent: "Three clients' multi-branch transactions over partly shared branches all commit, each branch merging the clients that wrote it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (6,60,600);",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b3\".public.t VALUES (6,60,600);",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b3\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b3\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/b1\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("automatic batch")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "disjoint updates and deletes merge on overlapping branches",
            intent: "Concurrent multi-branch transactions with disjoint updates and deletes merge on the branches they share.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b1\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/main\".public.t WHERE pk=3;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/b1\".public.t WHERE pk=3;",
                    expected: Expected::Ok,
                    expected_blocking: true,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b2\".public.t SET y=222 WHERE pk=2;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("100")],
                            &[T("2"), T("20"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("222")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("100")],
                            &[T("2"), T("20"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("222")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/b1\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("automatic batch")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "different columns of the same row merge on two branches",
            intent: "Concurrent multi-branch transactions that change different columns of a row merge on both branches.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b1\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/main\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b1\".public.t SET y=111 WHERE pk=1;",
                    expected: Expected::Ok,
                    expected_blocking: true,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/main\".public.t SET y=111 WHERE pk=1;",
                    expected: Expected::Ok,
                    expected_blocking: true,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("111")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("111")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("111")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("111")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/b1\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("automatic batch")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "late branch conflict rolls back the complete batch and permits retry",
            intent: "A conflict on one branch rolls back a multi-branch commit on every branch, and a retry succeeds.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/main\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/main\".public.t SET x=12 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b3\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("12"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/main\".public.t SET x=13 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("13"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("13"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/b1\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("automatic batch")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "delete update conflict does not publish nonconflicting branches",
            intent: "A delete-update conflict on one branch leaves the multi-branch commit's other branches unpublished.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/b1\".public.t WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b1\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    expected_blocking: true,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b3\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/b1\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("automatic batch")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "identical edits merge while preserving independent branch writes",
            intent: "Identical edits from two multi-branch transactions merge while each keeps its writes to other branches.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b1\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b1\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    expected_blocking: true,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/b1\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("automatic batch")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "savepoint restores edits across branches before merging another client",
            intent: "ROLLBACK TO SAVEPOINT undoes a multi-branch transaction's edits on every branch before another client's commit merges.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT keep;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b3\".public.t VALUES (6,60,600);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/b1\".public.t WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/main\".public.t SET x=99 WHERE pk=2;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT keep;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b3\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b3\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/b1\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("automatic batch")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "rollback and no-op commit leave other clients branch writes intact",
            intent: "A rolled back or empty multi-branch transaction leaves other clients' branch writes intact.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit_message = 'automatic batch';",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM \"postgres/b1\".pg_catalog.dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("automatic batch")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "multi branch opt in is session local",
            intent: "Turning dolt_multi_branch_commit on affects only the session that set it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am','setup');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('dolt_multi_branch_commit');",
                    expected: Expected::Rows {
                        columns: &[Column("current_setting", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot commit changes on more than one branch / database", ..E }),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_multi_branch_explicit_commit_transactions() {
    run_scripts(&[
        ScriptTest {
            name: "three clients merge partially overlapping branch sets",
            intent: "Three clients' multi-branch transactions over partly shared branches all commit, each branch merging the clients that wrote it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b3');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (6,60,600);",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b3\".public.t VALUES (6,60,600);",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b3\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b3\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("6"), T("60"), T("600")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "disjoint updates and deletes merge on overlapping branches",
            intent: "Concurrent multi-branch transactions with disjoint updates and deletes merge on the branches they share.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b3');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b1\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/main\".public.t WHERE pk=3;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/b1\".public.t WHERE pk=3;",
                    expected: Expected::Ok,
                    expected_blocking: true,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b2\".public.t SET y=222 WHERE pk=2;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("100")],
                            &[T("2"), T("20"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("222")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("100")],
                            &[T("2"), T("20"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("222")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "different columns of the same row merge on two branches",
            intent: "Concurrent multi-branch transactions that change different columns of a row merge on both branches.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b3');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b1\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/main\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b1\".public.t SET y=111 WHERE pk=1;",
                    expected: Expected::Ok,
                    expected_blocking: true,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/main\".public.t SET y=111 WHERE pk=1;",
                    expected: Expected::Ok,
                    expected_blocking: true,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("111")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("111")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("111")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("111")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "late branch conflict rolls back the complete batch and permits retry",
            intent: "A conflict on one branch rolls back a multi-branch commit on every branch, and a retry succeeds.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b3');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/main\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/main\".public.t SET x=12 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b3\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "WARNING", code: "25001", message: "there is already a transaction in progress", ..N }],
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("12"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/main\".public.t SET x=13 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("13"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("13"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "delete update conflict does not publish nonconflicting branches",
            intent: "A delete-update conflict on one branch leaves the multi-branch commit's other branches unpublished.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b3');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/b1\".public.t WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b1\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    expected_blocking: true,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b3\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "identical edits merge while preserving independent branch writes",
            intent: "Identical edits from two multi-branch transactions merge while each keeps its writes to other branches.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b3');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b1\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/b1\".public.t SET x=11 WHERE pk=1;",
                    expected: Expected::Ok,
                    expected_blocking: true,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("11"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "b",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "savepoint restores edits across branches before merging another client",
            intent: "ROLLBACK TO SAVEPOINT undoes a multi-branch transaction's edits on every branch before another client's commit merges.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b3');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT keep;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b3\".public.t VALUES (6,60,600);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM \"postgres/b1\".public.t WHERE pk=1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE \"postgres/main\".public.t SET x=99 WHERE pk=2;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT keep;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b3\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("4"), T("40"), T("400")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b3\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "rollback and no-op commit leave other clients branch writes intact",
            intent: "A rolled back or empty multi-branch transaction leaves other clients' branch writes intact.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, x int, y int);",
                "INSERT INTO t VALUES (1,10,100),(2,20,200),(3,30,300);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
                "SELECT dolt_branch('b3');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b3');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (4,40,400);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (5,50,500);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                            &[Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t AS OF 'HEAD' ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("100")],
                            &[T("2"), T("20"), T("200")],
                            &[T("3"), T("30"), T("300")],
                            &[T("5"), T("50"), T("500")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("multi-branch ref 2: a revision-qualified name read AS OF a revision relative to its branch, and the concurrency and failed-COMMIT expectations re-ported to Postgres' transaction semantics"),
            ..S
        },
        ScriptTest {
            name: "multi branch opt in is session local",
            intent: "Turning dolt_multi_branch_commit on affects only the session that set it.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am','setup');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_multi_branch_commit = 1;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('dolt_multi_branch_commit');",
                    expected: Expected::Rows {
                        columns: &[Column("current_setting", TEXT)],
                        rows: &[
                            &[T("off")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_transaction_commit = 1;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'session setting');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot commit changes on more than one branch / database", ..E }),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_multi_branch_commit_rows() {
    run_scripts(&[
        ScriptTest {
            name: "multi branch dolt_commit returns its branches' hashes in branch name order",
            intent: "With dolt_multi_branch_commit on, dolt_commit is a set of one hash for each branch the transaction wrote, in the order of the branches' names, each the branch's new head.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL dolt_multi_branch_commit = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE h AS SELECT row_number() OVER () AS n, hash FROM dolt_commit('-am', 'multi') AS c(hash);",
                    expected: Expected::Tag("SELECT 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n, hash = dolt_hashof('b1'), hash = dolt_hashof('b2'), hash = dolt_hashof('main') FROM h ORDER BY n;",
                    expected: Expected::Rows {
                        columns: &[Column("n", INT8), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("1"), T("t"), T("f"), T("f")],
                            &[T("2"), T("f"), T("t"), T("f")],
                            &[T("3"), T("f"), T("f"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_commit_all names each branch it committed and refuses what Dolt's refuses",
            intent: "dolt_commit_all commits every branch the transaction wrote whatever dolt_multi_branch_commit says, returning each branch's name in name order, refuses --branch, and names the branch it finds nothing to commit on.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT branch FROM dolt_commit_all('-am', 'all');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT)],
                        rows: &[
                            &[T("b2")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('--branch', 'b1', '-am', 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "--branch is not supported by dolt_commit_all", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'nothing');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "nothing to commit on postgres/main", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_commit_all('--skip-empty', '-am', 'nothing');",
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
            name: "a dolt_commit leaves its branch clean until the transaction writes it again",
            intent: "A branch counts as dirty, as Dolt's dirty working sets do, once the transaction writes it, even back to what it held, until a Dolt commit commits it, while a branch the transaction makes starts clean and a rollback to a savepoint gives each branch back the state it had there.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY);",
                "SELECT dolt_commit('-Am', 'setup');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so Doltgrust's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'main') IS NOT NULL;",
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
                    query: "SELECT count(*) = 1 FROM dolt_checkout('-b', 'b2');",
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
                    query: "INSERT INTO t VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b2') IS NOT NULL;",
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
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--branch', 'main', '-am', 'main 2') IS NOT NULL;",
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
                    query: "UPDATE t SET pk = 20 WHERE pk = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET pk = 2 WHERE pk = 20;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'two');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot commit changes on more than one branch / database", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'b2 again') IS NOT NULL;",
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
                    query: "ROLLBACK TO SAVEPOINT s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (7);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'two');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Cannot commit changes on more than one branch / database", ..E }),
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
    ]);
}
