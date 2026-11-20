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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_multi_branch_transaction_queries.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_multi_branch_working_set_transaction_tests() {
    run_scripts(&[
        ScriptTest {
            name: "three clients merge partially overlapping branch sets",
            intent: "Three clients' transactions writing partly shared branches all commit, each branch merging the clients that wrote it.",
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
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "disjoint updates and deletes merge on overlapping branches",
            intent: "Concurrent transactions with disjoint updates and deletes over several branches merge on the branches they share.",
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
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "different columns of the same row merge on two branches",
            intent: "Concurrent transactions that change different columns of a row on two branches merge on both.",
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
                    client: "b",
                    ..A
                },
            ],
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "late branch conflict rolls back the complete batch and permits retry",
            intent: "A conflict on one branch rolls back a transaction's writes to every branch, and a retry succeeds.",
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
                        ],
                        tag: "SELECT 3",
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
            ],
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "delete update conflict does not publish nonconflicting branches",
            intent: "A delete-update conflict on one branch leaves the transaction's other branches unpublished.",
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
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "identical edits merge while preserving independent branch writes",
            intent: "Identical edits from two transactions merge while each keeps its writes to other branches.",
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
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
            ],
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "savepoint restores edits across branches before merging another client",
            intent: "ROLLBACK TO SAVEPOINT undoes a transaction's edits on every branch before another client's commit merges.",
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
                    query: "RELEASE SAVEPOINT keep;",
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
                        ],
                        tag: "SELECT 3",
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
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "rollback and no-op commit leave other clients branch writes intact",
            intent: "A rolled back or empty transaction leaves other clients' writes to several branches intact.",
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
                        ],
                        tag: "SELECT 3",
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
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
    ]);
}

#[test]
fn test_multi_branch_head_transaction_tests() {
    run_scripts(&[
        ScriptTest {
            name: "three clients merge partially overlapping branch sets",
            intent: "Three clients' transactions writing partly shared branches all commit, each branch merging the clients that wrote it.",
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
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("b2"), Any],
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
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b2"), Any],
                            &[T("b3"), Any],
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
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "disjoint updates and deletes merge on overlapping branches",
            intent: "Concurrent transactions with disjoint updates and deletes over several branches merge on the branches they share.",
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
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("b2"), Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("main"), Any],
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
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "different columns of the same row merge on two branches",
            intent: "Concurrent transactions that change different columns of a row on two branches merge on both.",
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
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("main"), Any],
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
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "late branch conflict rolls back the complete batch and permits retry",
            intent: "A conflict on one branch rolls back a transaction's writes to every branch, and a retry succeeds.",
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
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b3"), Any],
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
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
                    query: "SELECT * FROM \"postgres/b1\".public.t AS OF 'HEAD' ORDER BY pk;",
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
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("main"), Any],
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
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "delete update conflict does not publish nonconflicting branches",
            intent: "A delete-update conflict on one branch leaves the transaction's other branches unpublished.",
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
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("b3"), Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
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
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "identical edits merge while preserving independent branch writes",
            intent: "Identical edits from two transactions merge while each keeps its writes to other branches.",
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
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("main"), Any],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("b2"), Any],
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
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "savepoint restores edits across branches before merging another client",
            intent: "ROLLBACK TO SAVEPOINT undoes a transaction's edits on every branch before another client's commit merges.",
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
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("b2"), Any],
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
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("main"), Any],
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
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
        ScriptTest {
            name: "rollback and no-op commit leave other clients branch writes intact",
            intent: "A rolled back or empty transaction leaves other clients' writes to several branches intact.",
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
                    query: "SELECT * FROM dolt_commit_all('-am', 'batch');",
                    expected: Expected::Rows {
                        columns: &[Column("branch", TEXT), Column("hash", TEXT)],
                        rows: &[
                            &[T("b1"), Any],
                            &[T("b2"), Any],
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
            skip: Some("written for Dolt's transactions, which merge at commit and never wait: needs a re-port to Postgres' row locks (Appendix C transaction concurrency), and dolt_commit_all, which lands with multi-branch commits"),
            ..S
        },
    ]);
}
