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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_transaction_queries.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_dolt_transaction_tests() {
    run_scripts(&[
        ScriptTest {
            name: "duplicate inserts, autocommit on",
            intent: "A client's insert of a key another client's insert already committed fails as a duplicate key.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int);",
                "INSERT INTO t VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"t_pkey\"", detail: "Key (x)=(2) already exists.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "duplicate inserts, autocommit off",
            intent: "Two transactions that insert the same row: the second waits for the first and fails as a duplicate key once the first commits.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int);",
                "INSERT INTO t VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"t_pkey\"", detail: "Key (x)=(2) already exists.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' unique check waits for the other transaction's uncommitted insert of the key and fails once it commits, where Dolt merges the transactions at commit, Appendix C \"transaction concurrency\"")),
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
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    dolt: Some(Dolt::Differs("the failed statement aborted the transaction, so COMMIT rolls it back, where Dolt's commit merges, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting inserts",
            intent: "Two transactions that insert the same key with different rows: the second fails as a duplicate key, and its row can't be inserted afterwards.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int);",
                "INSERT INTO t VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 3);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"t_pkey\"", detail: "Key (x)=(2) already exists.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' unique check waits for the other transaction's uncommitted insert of the key and fails once it commits, where Dolt merges the transactions at commit, Appendix C \"transaction concurrency\"")),
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
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    dolt: Some(Dolt::Differs("the failed statement aborted the transaction, so COMMIT rolls it back, where Dolt's commit merges, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 3);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"t_pkey\"", detail: "Key (x)=(2) already exists.", schema: "public", table: "t", constraint: "t_pkey", ..E }),
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "duplicate updates, autocommit on",
            intent: "Two clients' identical updates outside transaction blocks both succeed and leave the updated rows.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE t SET y = 2;",
                    expected: Expected::Tag("UPDATE 2"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET y = 2;",
                    expected: Expected::Tag("UPDATE 2"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "duplicate updates, autocommit off",
            intent: "Two transactions making the same update both commit, the second waiting for the first's row locks.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET y = 2;",
                    expected: Expected::Tag("UPDATE 2"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET y = 2;",
                    expected: Expected::Tag("UPDATE 2"),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' row lock makes the statement wait for the other transaction to end and then act on the rows it committed, where Dolt's transactions never wait and merge at commit, Appendix C \"transaction concurrency\"")),
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
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting updates",
            intent: "Two transactions updating the same rows to different values: the second waits for the first and its update wins once both commit.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET y = 3;",
                    expected: Expected::Tag("UPDATE 2"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET y = 4;",
                    expected: Expected::Tag("UPDATE 2"),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' row lock makes the statement wait for the other transaction to end and then act on the rows it committed, where Dolt's transactions never wait and merge at commit, Appendix C \"transaction concurrency\"")),
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
                    client: "b",
                    dolt: Some(Dolt::Differs("the second transaction's update waited for the first and committed after it, where Dolt's commit fails on the conflicting cells, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("4")],
                            &[T("2"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("the second transaction's update committed after the first, where Dolt keeps the first's rows, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("4")],
                            &[T("2"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("the second transaction's update committed after the first, where Dolt keeps the first's rows, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    dolt: Some(Dolt::Differs("the transaction committed, so no transaction is in progress, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("4")],
                            &[T("2"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("the second transaction's update committed after the first, where Dolt keeps the first's rows, Appendix C \"transaction concurrency\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "non overlapping updates (diff rows)",
            intent: "Two transactions updating different rows both commit, and every client sees both updates.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET y = 3 WHERE x = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET y = 4 WHERE x = 2;",
                    expected: Expected::Tag("UPDATE 1"),
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
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("3")],
                            &[T("2"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("3")],
                            &[T("2"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "non overlapping updates (diff cols)",
            intent: "Two transactions updating different columns of the same rows both commit, the second waiting for the first, and both updates survive.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int, z int);",
                "INSERT INTO t VALUES (1, 1, 1), (2, 2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET y = 2;",
                    expected: Expected::Tag("UPDATE 2"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET z = 3;",
                    expected: Expected::Tag("UPDATE 2"),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' row lock makes the statement wait for the other transaction to end and then act on the rows it committed, where Dolt's transactions never wait and merge at commit, Appendix C \"transaction concurrency\"")),
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
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("z", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                            &[T("2"), T("2"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("z", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                            &[T("2"), T("2"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "duplicate deletes, autocommit on",
            intent: "A client's delete of rows another client already deleted deletes nothing.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE y = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE y = 2;",
                    expected: Expected::Tag("DELETE 0"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "duplicate deletes, autocommit off",
            intent: "Two transactions deleting the same row both commit, the second waiting for the first and deleting nothing.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE y = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE y = 2;",
                    expected: Expected::Tag("DELETE 0"),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' row lock makes the statement wait for the other transaction to end and then act on the rows it committed, where Dolt's transactions never wait and merge at commit, Appendix C \"transaction concurrency\"")),
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
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "non overlapping deletes",
            intent: "Two transactions deleting different rows both commit, and every client sees both deletes.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int);",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE y = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE y = 3;",
                    expected: Expected::Tag("DELETE 1"),
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
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicting delete and update",
            intent: "A delete that waits for another transaction's update of its row finds the row no longer matches and deletes nothing.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET y = 3 WHERE y = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE y = 2;",
                    expected: Expected::Tag("DELETE 0"),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' row lock makes the statement wait for the other transaction to end and then act on the rows it committed, where Dolt's transactions never wait and merge at commit, Appendix C \"transaction concurrency\"")),
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
                    client: "b",
                    dolt: Some(Dolt::Differs("the delete waited for the update and matched no row, so the commit succeeds, where Dolt's commit fails on the conflicting row, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    dolt: Some(Dolt::Differs("the transaction committed, so no transaction is in progress, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete in one client, insert into another",
            intent: "A transaction's delete of every row waits for another transaction's delete, and its insert of a deleted key afterwards survives both commits.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int);",
                "INSERT INTO t VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE y = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t;",
                    expected: Expected::Tag("DELETE 1"),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' row lock makes the statement wait for the other transaction to end and then act on the rows it committed, where Dolt's transactions never wait and merge at commit, Appendix C \"transaction concurrency\"; the first transaction's commit moves before the insert, which can't run while the delete waits")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1,1);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("the transactions ran one after the other, so the reinserted row survives, where Dolt's merge of the two deletes leaves no rows, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("the transactions ran one after the other, so the reinserted row survives, where Dolt's merge of the two deletes leaves no rows, Appendix C \"transaction concurrency\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "multiple client edit session",
            intent: "Three transactions' updates and deletes: an update that waits for another transaction's change of its row matches nothing, and each client sees the committed rows.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY, y int, z int);",
                "INSERT INTO t VALUES (1, 1, 1), (2, 2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET y = 3 WHERE y = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE y = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET z = 4 WHERE y = 2;",
                    expected: Expected::Tag("UPDATE 0"),
                    expected_blocking: true,
                    client: "c",
                    dolt: Some(Dolt::Differs("Postgres' row lock makes the statement wait for the other transaction to end and then act on the rows it committed, where Dolt's transactions never wait and merge at commit, Appendix C \"transaction concurrency\"")),
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
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("z", INT4)],
                        rows: &[
                            &[T("2"), T("3"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("z", INT4)],
                        rows: &[
                            &[T("2"), T("3"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("z", INT4)],
                        rows: &[
                            &[T("2"), T("3"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("z", INT4)],
                        rows: &[
                            &[T("2"), T("3"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("the waiting update matched no row, where Dolt merges it into the other transactions' rows, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("z", INT4)],
                        rows: &[
                            &[T("2"), T("3"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("the waiting update matched no row, where Dolt merges it into the other transactions' rows, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("z", INT4)],
                        rows: &[
                            &[T("2"), T("3"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "c",
                    dolt: Some(Dolt::Differs("the waiting update matched no row, where Dolt merges it into the other transactions' rows, Appendix C \"transaction concurrency\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "edits from different clients to table with out of order primary key set",
            intent: "An update in a transaction of a key column another client just changed and committed matches nothing, and a later insert of the changed key fails as a duplicate.",
            set_up_script: &[
                "CREATE TABLE test (x int, y int, z int, PRIMARY KEY(z, y));",
                "INSERT INTO test VALUES (1, 1, 1), (2, 2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET y = 3 WHERE y = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET y = 5 WHERE y = 2;",
                    expected: Expected::Tag("UPDATE 0"),
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("z", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("3"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4), Column("z", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("1")],
                            &[T("2"), T("3"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (4,3,2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_pkey\"", detail: "Key (z, y)=(2, 3) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Merge unresolved FKs after resolved FKs were committed",
            intent: "A table created with a foreign key in one transaction keeps its foreign key after a later transaction commits alongside another client's commit.",
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ref (id varchar(100) PRIMARY KEY, status int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE t (id int, ref_id varchar(100), FOREIGN KEY (ref_id) REFERENCES ref(id));",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("SET foreign_key_checks is MySQL's and is left out; Postgres checks every foreign key when it is created")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET ref_id = '42' WHERE ref_id::int > 100000;",
                    expected: Expected::Tag("UPDATE 0"),
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("MySQL compares and assigns between varchar and int without casts, which Postgres needs written")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE foo (i int);",
                    expected: Expected::Tag("CREATE TABLE"),
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 't'::regclass ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("t_ref_id_fkey"), T("FOREIGN KEY (ref_id) REFERENCES ref(id)")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_constraint shows the table's foreign key")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid = 't'::regclass ORDER BY conname;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_get_constraintdef", TEXT)],
                        rows: &[
                            &[T("t_ref_id_fkey"), T("FOREIGN KEY (ref_id) REFERENCES ref(id)")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; pg_constraint shows the table's foreign key")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "TRANSACTION ISOLATION READ-COMMITTED does not break AUTOCOMMIT=OFF",
            intent: "Setting the session's isolation level to READ COMMITTED leaves the transaction block working, with its savepoints.",
            assertions: &[
                ScriptTestAssertion {
                    query: "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL READ COMMITTED;",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT current_setting('transaction_isolation');",
                    expected: Expected::Rows {
                        columns: &[Column("current_setting", TEXT)],
                        rows: &[
                            &[T("read committed")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("@@autocommit is MySQL's and is left out; the transaction block stands for autocommit off")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT abc;",
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "RELEASE SAVEPOINT abc;",
                    expected: Expected::Tag("RELEASE"),
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "non-ff commit merge with multiple indexes on a column",
            intent: "Two transactions inserting into a table with two indexes on the same column both commit, and a later transaction sees every row.",
            set_up_script: &[
                "CREATE TABLE t1 (pk int PRIMARY KEY, val int);",
                "CREATE INDEX i1 ON t1 (val);",
                "CREATE UNIQUE INDEX u1 ON t1 (val);",
                "INSERT INTO t1 VALUES (1, 1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (3, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_dolt_stored_procedure_transaction_tests() {
    run_scripts(&[
        ScriptTest {
            name: "committed conflicts are seen by other sessions",
            intent: "A merge's conflicts that one transaction commits with dolt_allow_commit_conflicts become visible to other sessions' new transactions, and an aborted merge removes them.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "SELECT dolt_commit('-a', '-m', 'Step 1');",
                "SELECT * FROM dolt_checkout('-b', 'feature-branch');",
                "INSERT INTO test VALUES (1, 1);",
                "UPDATE test SET val=1000 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'this is a normal commit');",
                "SELECT * FROM dolt_checkout('main');",
                "UPDATE test SET val=1001 WHERE pk=0;",
                "SELECT dolt_commit('-a', '-m', 'update a value');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
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
                    client: "a",
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts_test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts_test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 1;",
                    expected: Expected::Tag("SET"),
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
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25001", message: "there is already a transaction in progress", ..N }],
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts_test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
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
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25001", message: "there is already a transaction in progress", ..N }],
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = 0;",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('main', 'feature-branch');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('feature-branch');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts can be resolved using the dolt_conflicts and dolt_schema_conflicts tables only inside an explicit transaction (BEGIN ... COMMIT). Alternatively, to commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts_test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts_test;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_commit with one table, no merge conflict, no unstaged changes",
            intent: "Two transactions that update different rows and each dolt_commit both get their commits, the second built on the first, with both updates in HEAD.",
            set_up_script: &[
                "CREATE TABLE users (id int PRIMARY KEY, name varchar(32));",
                "INSERT INTO users VALUES (1, 'tim'), (2, 'jim');",
                "SELECT dolt_commit('-A', '-m', 'initial commit');",
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
                    query: "UPDATE users SET name = 'tim2' WHERE name = 'tim';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE users SET name = 'jim2' WHERE name = 'jim';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-A', '-m', 'update tim');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-A', '-m', 'update jim');",
                    expected: Expected::Ok,
                    client: "b",
                    expected_blocking: true,
                    dolt: Some(Dolt::Differs("a dolt_commit in a transaction block holds the branch's head until the transaction ends, so the second transaction's dolt_commit waits for the first, where Dolt's dolt_commit ends its transaction, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    dolt: Some(Dolt::Differs("Dolt's dolt_commit ends the transaction, which a Postgres function can't, so the first transaction commits here to let the waiting dolt_commit go on, Appendix C \"ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    dolt: Some(Dolt::Differs("Dolt's dolt_commit ends the transaction, which a Postgres function can't, so the second transaction commits here, Appendix C \"ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block\"")),
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
                    client: "a",
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
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tim2")],
                            &[T("2"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tim2")],
                            &[T("2"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("update jim")],
                            &[T("update tim")],
                            &[T("initial commit")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users AS OF 'HEAD' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tim2")],
                            &[T("2"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "mix of dolt_commit and normal commit",
            intent: "A transaction's dolt_commit -A after another transaction's plain commit commits the working set it sees, with the other transaction's change.",
            set_up_script: &[
                "CREATE TABLE users (id int PRIMARY KEY, name varchar(32));",
                "INSERT INTO users VALUES (1, 'tim'), (2, 'jim');",
                "SELECT dolt_commit('-A', '-m', 'initial commit');",
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
                    query: "UPDATE users SET name = 'tim2' WHERE name = 'tim';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE users SET name = 'jim2' WHERE name = 'jim';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-A', '-m', 'update jim');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    dolt: Some(Dolt::Differs("Dolt's dolt_commit ends the transaction, which a Postgres function can't, so the transaction commits here, Appendix C \"ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block\"")),
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
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\", so the dolt_commit -A includes the other transaction's committed update")),
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
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\", so the dolt_commit -A includes the other transaction's committed update")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tim2")],
                            &[T("2"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tim2")],
                            &[T("2"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_name, to_name FROM dolt_diff('HEAD', 'WORKING', 'users') ORDER BY from_id, to_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_name", VARCHAR), Column("to_name", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\", so the dolt_commit -A includes the other transaction's committed update")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_name, to_name FROM dolt_diff('HEAD', 'WORKING', 'users') ORDER BY from_id, to_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_name", VARCHAR), Column("to_name", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\", so the dolt_commit -A includes the other transaction's committed update")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "staged change in working set",
            intent: "Two transactions that each stage an update of a different row both keep their staged changes after committing.",
            set_up_script: &[
                "CREATE TABLE users (id int PRIMARY KEY, name varchar(32));",
                "INSERT INTO users VALUES (1, 'tim'), (2, 'jim');",
                "SELECT dolt_commit('-A', '-m', 'initial commit');",
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
                    query: "UPDATE users SET name = 'tim2' WHERE name = 'tim';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE users SET name = 'jim2' WHERE name = 'jim';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('users');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('users');",
                    expected: Expected::Ok,
                    client: "b",
                    expected_blocking: true,
                    dolt: Some(Dolt::Differs("a dolt_add in a transaction block holds the branch's staged root until the transaction ends, so the other transaction's dolt_add waits for it, where Dolt's transactions merge at commit, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tim2")],
                            &[T("2"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tim2")],
                            &[T("2"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.users"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("dolt_status names a table by its schema-qualified name, STORAGE_DESIGN §IV.3a")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.users"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("dolt_status names a table by its schema-qualified name, STORAGE_DESIGN §IV.3a")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_name, to_name FROM dolt_diff('HEAD', 'STAGED', 'users') ORDER BY from_id, to_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_name", VARCHAR), Column("to_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("tim"), T("tim2")],
                            &[T("2"), T("2"), T("jim"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_name, to_name FROM dolt_diff('HEAD', 'STAGED', 'users') ORDER BY from_id, to_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_name", VARCHAR), Column("to_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("tim"), T("tim2")],
                            &[T("2"), T("2"), T("jim"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "staged and unstaged changes in working set",
            intent: "Two transactions that each stage an update and then change the row again keep both staged changes and both unstaged ones after committing.",
            set_up_script: &[
                "CREATE TABLE users (id int PRIMARY KEY, name varchar(32));",
                "INSERT INTO users VALUES (1, 'tim'), (2, 'jim');",
                "SELECT dolt_commit('-A', '-m', 'initial commit');",
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
                    query: "UPDATE users SET name = 'tim2' WHERE name = 'tim';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE users SET name = 'jim2' WHERE name = 'jim';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('users');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('users');",
                    expected: Expected::Ok,
                    client: "b",
                    expected_blocking: true,
                    dolt: Some(Dolt::Differs("a dolt_add in a transaction block holds the branch's staged root until the transaction ends, so the other transaction's dolt_add waits for it, where Dolt's transactions merge at commit, Appendix C \"transaction concurrency\"; the first transaction's update and commit move before the second's update, which can't run while its dolt_add waits")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE users SET name = 'tim3' WHERE name = 'tim2';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE users SET name = 'jim3' WHERE name = 'jim2';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tim3")],
                            &[T("2"), T("jim3")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tim3")],
                            &[T("2"), T("jim3")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.users"), T("t"), T("modified")],
                            &[T("public.users"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("dolt_status names a table by its schema-qualified name, STORAGE_DESIGN §IV.3a")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.users"), T("t"), T("modified")],
                            &[T("public.users"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("dolt_status names a table by its schema-qualified name, STORAGE_DESIGN §IV.3a")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_name, to_name FROM dolt_diff('HEAD', 'STAGED', 'users') ORDER BY from_id, to_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_name", VARCHAR), Column("to_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("tim"), T("tim2")],
                            &[T("2"), T("2"), T("jim"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_name, to_name FROM dolt_diff('HEAD', 'STAGED', 'users') ORDER BY from_id, to_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_name", VARCHAR), Column("to_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("tim"), T("tim2")],
                            &[T("2"), T("2"), T("jim"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_name, to_name FROM dolt_diff('HEAD', 'WORKING', 'users') ORDER BY from_id, to_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_name", VARCHAR), Column("to_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("tim"), T("tim3")],
                            &[T("2"), T("2"), T("jim"), T("jim3")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_name, to_name FROM dolt_diff('HEAD', 'WORKING', 'users') ORDER BY from_id, to_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_name", VARCHAR), Column("to_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("tim"), T("tim3")],
                            &[T("2"), T("2"), T("jim"), T("jim3")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_name, to_name FROM dolt_diff('STAGED', 'WORKING', 'users') ORDER BY from_id, to_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_name", VARCHAR), Column("to_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("tim2"), T("tim3")],
                            &[T("2"), T("2"), T("jim2"), T("jim3")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_name, to_name FROM dolt_diff('STAGED', 'WORKING', 'users') ORDER BY from_id, to_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_name", VARCHAR), Column("to_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("tim2"), T("tim3")],
                            &[T("2"), T("2"), T("jim2"), T("jim3")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "staged changes in working set, dolt_add and dolt_commit on top of it",
            intent: "A transaction's dolt_add and dolt_commit after another transaction committed its staged change commit both changes.",
            set_up_script: &[
                "CREATE TABLE users (id int PRIMARY KEY, name varchar(32));",
                "INSERT INTO users VALUES (1, 'tim'), (2, 'jim');",
                "SELECT dolt_commit('-A', '-m', 'initial commit');",
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
                    query: "UPDATE users SET name = 'tim2' WHERE name = 'tim';",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE users SET name = 'jim2' WHERE name = 'jim';",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('users');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('users');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tim2")],
                            &[T("2"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'jim2 commit');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tim2")],
                            &[T("2"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM users AS OF 'HEAD' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("tim2")],
                            &[T("2"), T("jim2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
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
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_name, to_name FROM dolt_diff('HEAD', 'STAGED', 'users') ORDER BY from_id, to_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_name", VARCHAR), Column("to_name", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_name, to_name FROM dolt_diff('HEAD', 'WORKING', 'users') ORDER BY from_id, to_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_name", VARCHAR), Column("to_name", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "call dolt_commit commits staged stuff, merges with working set and branch head",
            intent: "A dolt_commit commits only the staged tables, onto the head another transaction committed, and leaves the unstaged rows and tables in the working set.",
            set_up_script: &[
                "CREATE TABLE t1 (id int PRIMARY KEY, val int);",
                "CREATE TABLE t2 (id int PRIMARY KEY, val int);",
                "INSERT INTO t1 VALUES (1, 1), (2, 2);",
                "INSERT INTO t2 VALUES (1, 1), (2, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'initial commit of t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
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
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (3, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (4, 4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (3, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (4, 4);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (5, 5);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (6, 6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "c",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'add 3 to t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    dolt: Some(Dolt::Differs("Dolt's dolt_commit ends the transaction, which a Postgres function can't, so the transaction commits here, Appendix C \"ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER BY id ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("6"), T("6")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY id ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("5"), T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 AS OF 'HEAD' ORDER BY id ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("a dolt_add in a transaction block holds the branch's staged root until the transaction ends, so this dolt_add and the insert after it move after the first transaction's commit, where Dolt's transactions run side by side and merge at commit, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (6, 6);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-m', 'add 4 to t1');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    dolt: Some(Dolt::Differs("Dolt's dolt_commit ends the transaction, which a Postgres function can't, so the transaction commits here, Appendix C \"ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t2 ORDER BY id ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("6"), T("6")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY id ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                            &[T("6"), T("6")],
                        ],
                        tag: "SELECT 6",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 AS OF 'HEAD' ORDER BY id ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\", so the second dolt_add stages the first transaction's committed row 5 too")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t2"), T("f"), T("new table")],
                            &[T("public.t1"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("dolt_status names a table by its schema-qualified name, STORAGE_DESIGN §IV.3a")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.t2"), T("f"), T("new table")],
                            &[T("public.t1"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("dolt_status names a table by its schema-qualified name, STORAGE_DESIGN §IV.3a")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_val, to_val FROM dolt_diff('HEAD', 'WORKING', 't1') ORDER BY from_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_val", INT4), Column("to_val", INT4)],
                        rows: &[
                            &[Null, T("6"), Null, T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\", so the second dolt_add staged row 5 and HEAD has every row but 6")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT from_id, to_id, from_val, to_val FROM dolt_diff('HEAD', 'WORKING', 't1') ORDER BY from_id;",
                    expected: Expected::Rows {
                        columns: &[Column("from_id", INT4), Column("to_id", INT4), Column("from_val", INT4), Column("to_val", INT4)],
                        rows: &[
                            &[Null, T("6"), Null, T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\", so the second dolt_add staged row 5 and HEAD has every row but 6")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "amend commit in transaction fails when another client merges into the branch",
            intent: "A transaction's dolt_commit --amend fails once another client merged into the branch, and the merge stays the head.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val varchar(30));",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (1, 'initial'), (2, 'second');",
                "SELECT dolt_commit('-a', '-m', 'initial commit');",
                "SELECT * FROM dolt_checkout('-b', 'side');",
                "INSERT INTO test VALUES (100, 'side work');",
                "SELECT dolt_commit('-a', '-m', 'side work');",
                "SELECT * FROM dolt_checkout('main');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET val = 'stale change' WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--no-ff', 'side');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = dolt_hashof('main');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\", so HEAD is the head the other client committed")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '--amend', '-m', 'stale amend');",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("Merge branch 'side' into main")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log WHERE message = 'stale amend';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val FROM test WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("val", VARCHAR)],
                        rows: &[
                            &[T("initial")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val FROM test WHERE pk = 100;",
                    expected: Expected::Rows {
                        columns: &[Column("val", VARCHAR)],
                        rows: &[
                            &[T("side work")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "amend commit in transaction fails when another client moves the branch head",
            intent: "A transaction's dolt_commit --amend fails whenever another client moved the branch head since the transaction began, by a commit, an empty commit, or an amend.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val varchar(30));",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (1, 'initial'), (2, 'second');",
                "SELECT dolt_commit('-a', '-m', 'initial commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET val = 'stale change' WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (3, 'concurrent row');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '-m', 'concurrent commit');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = dolt_hashof('main');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement, and the commit, a snapshot of everything committed before it, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\", so HEAD is the head the other client committed")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '--amend', '-m', 'stale amend');",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("MySQL's deadlock error rolls the transaction back, where Postgres leaves the failed transaction block open until ROLLBACK")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET val = 'stale change' WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('--allow-empty', '-m', 'empty head move');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '--amend', '-m', 'stale amend');",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("MySQL's deadlock error rolls the transaction back, where Postgres leaves the failed transaction block open until ROLLBACK")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET val = 'stale change' WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET val = 'concurrent amend' WHERE pk = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '--amend', '-m', 'concurrent amend');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    dolt: Some(Dolt::Differs("A dolt_commit in an explicit transaction moves the branch when the transaction commits (the owner, 2026-10-25), where Dolt's moves it at once, so client b commits before client a's amend")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '--amend', '-m', 'stale amend');",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("concurrent amend")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log WHERE message = 'stale amend';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_log WHERE message = 'empty head move';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val FROM test WHERE pk = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("val", VARCHAR)],
                        rows: &[
                            &[T("initial")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val FROM test WHERE pk = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("val", VARCHAR)],
                        rows: &[
                            &[T("concurrent amend")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "amend commit tolerates ignored table changes from both clients",
            intent: "A transaction's dolt_commit --amend succeeds while both clients changed only ignored tables, which stay out of the commit and keep their rows.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val varchar(30));",
                "INSERT INTO dolt_ignore VALUES ('ignored_*', TRUE);",
                "CREATE TABLE ignored_t (pk int PRIMARY KEY);",
                "INSERT INTO test VALUES (1, 'initial');",
                "SELECT dolt_commit('-Am', 'initial commit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test SET val = 'amend me' WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ignored_t VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ignored_a (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ignored_t VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ignored_b (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = dolt_hashof('main');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-a', '--amend', '-m', 'amended');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("amended")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM ignored_t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT val FROM test AS OF dolt_hashof('HEAD');",
                    expected: Expected::Rows {
                        columns: &[Column("val", VARCHAR)],
                        rows: &[
                            &[T("amend me")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM dolt_status_ignored WHERE ignored = TRUE ORDER BY table_name;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT)],
                        rows: &[
                            &[T("public.ignored_a")],
                            &[T("public.ignored_b")],
                            &[T("public.ignored_t")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
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
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT table_name FROM information_schema.tables WHERE table_schema = CURRENT_SCHEMA() ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", NAME)],
                        rows: &[
                            &[T("test")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            skip: Some("the session's ignored tables stay in its working set, so information_schema lists them, where the Doltgres expectation lists test alone"),
            ..S
        },
    ]);
}

#[test]
fn test_dolt_constraint_violation_transaction_tests() {
    run_scripts(&[
        ScriptTest {
            name: "Constraint violations created by concurrent writes should cause a rollback",
            intent: "A child row inserted against a parent row another transaction deletes fails its foreign key once the delete commits, and inserting it again fails too.",
            set_up_script: &[
                "CREATE TABLE parent (pk int PRIMARY KEY);",
                "CREATE TABLE child (pk int PRIMARY KEY, parent_fk int, FOREIGN KEY (parent_fk) REFERENCES parent (pk));",
                "INSERT INTO parent VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
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
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM parent WHERE pk = 1;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (1, 1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"child\" violates foreign key constraint \"child_parent_fk_fkey\"", detail: "Key (parent_fk)=(1) is not present in table \"parent\".", schema: "public", table: "child", constraint: "child_parent_fk_fkey", ..E }),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' foreign key check waits for the transaction deleting the parent row and fails once it commits, where Dolt finds the violation when the second transaction commits, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    dolt: Some(Dolt::Differs("the failed insert aborted the transaction, so COMMIT rolls it back, where Dolt's commit fails on the violation, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO child VALUES (1, 1);",
                    expected: Expected::Error(Diagnostic { code: "23503", message: "insert or update on table \"child\" violates foreign key constraint \"child_parent_fk_fkey\"", detail: "Key (parent_fk)=(1) is not present in table \"parent\".", schema: "public", table: "child", constraint: "child_parent_fk_fkey", ..E }),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Constraint violations created by DOLT_MERGE should cause a roll back",
            intent: "A merge that leaves unique key violations fails and rolls back, leaving no violations and no merge to abort.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table');",
                "SELECT * FROM dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (2, 1);",
                "SELECT dolt_commit('-am', 'right edit');",
                "SELECT * FROM dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 1);",
                "SELECT dolt_commit('-am', 'left edit');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('right');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Committing this transaction resulted in a working set with constraint violations, transaction rolled back. This constraint violation may be the result of a previous merge or the result of transaction sequencing. Constraint violations from a merge can be resolved using the dolt_constraint_violations table before committing the transaction. To allow transactions to be committed with constraint violations from a merge or transaction sequencing set dolt_force_transaction_commit to on.\nConstraint violations: \nType: Unique Key Constraint Violation,\n	Name: t_col1_key,\n	Columns: [col1] (2 row(s))", ..E }),
                    client: "a",
                    dolt: Some(Dolt::Differs("violation_info names the constraint, its index, and its actions as Postgres does, and the error lists them so (Appendix C); the message also names the setting as Postgres sets it, with no @@")),
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM DOLT_CONSTRAINT_VIOLATIONS;",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_violations", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('--abort');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "fatal: There is no merge to abort", ..E }),
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a transaction commit that is a fast-forward produces no constraint violations",
            intent: "A transaction that deletes a referenced parent row with foreign key checks off commits when no other transaction committed first.",
            set_up_script: &[
                "CREATE TABLE parent (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent (v1);",
                "CREATE TABLE child (pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_add('.');",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1);",
                "SELECT dolt_commit('-am', 'MC1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET session_replication_role = replica;",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("SET foreign_key_checks = 0 stands as session_replication_role = replica, which also turns off Postgres' foreign key checks; MySQL's foreign key may reference a column with a plain index, and Postgres needs a unique one, so parent's index on v1 is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM PARENT WHERE v1 = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a transaction commit that is a three-way merge produces constraint violations",
            intent: "A transaction committing after another transaction deleted a referenced parent row with foreign key checks off fails on the violation the merge finds.",
            set_up_script: &[
                "CREATE TABLE parent (pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON parent (v1);",
                "CREATE TABLE child (pk bigint PRIMARY KEY, v1 bigint);",
                "SELECT dolt_add('.');",
                "INSERT INTO parent VALUES (10, 1), (20, 2);",
                "INSERT INTO child VALUES (1, 1), (2, 2);",
                "ALTER TABLE child ADD CONSTRAINT fk_name FOREIGN KEY (v1) REFERENCES parent (v1);",
                "SELECT dolt_commit('-am', 'MC1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET session_replication_role = replica;",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    dolt: Some(Dolt::MySqlOnly("SET foreign_key_checks = 0 stands as session_replication_role = replica, which also turns off Postgres' foreign key checks; MySQL's foreign key may reference a column with a plain index, and Postgres needs a unique one, so parent's index on v1 is unique")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM PARENT WHERE v1 = 2;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO parent VALUES (30, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' transactions never merge at commit, so committing after another transaction's commit checks no constraints again, where Dolt's merge at commit finds the violation, Appendix C \"transaction concurrency\"")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_branch_isolation_tests() {
    run_scripts(&[
        ScriptTest {
            name: "clients can't see changes on other branch working sets made since transaction start",
            intent: "A transaction keeps reading another branch's working set as it was when the transaction began, through revision names and AS OF, until it starts a new transaction.",
            set_up_script: &[
                "CREATE TABLE t1 (a int);",
                "INSERT INTO t1 VALUES (1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Ok,
                    client: "a",
                    dolt: Some(Dolt::Differs("Dolt's transactions read one snapshot as REPEATABLE READ does, where Postgres' default READ COMMITTED takes one per statement, so the port asks for REPEATABLE READ")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM \"postgres/b1\".public.t1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("a REPEATABLE READ transaction takes its snapshot at its first statement, where Dolt's takes it at START TRANSACTION, so the port reads once first")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (2);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 AS OF 'b1' ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    dolt: Some(Dolt::Differs("MySQL's START TRANSACTION commits the transaction in progress, where Postgres' warns and keeps it, so the port commits first")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 AS OF 'b1' ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "atomic writes to three branches merge concurrent working sets",
            intent: "Two transactions' writes to three branches through revision names both commit, and each branch has both transactions' rows.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
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
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (3, 30);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (3, 30);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (3, 30);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t AS OF 'HEAD';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflict on one branch rolls back every branch",
            intent: "A transaction whose write conflicts on one branch fails to commit and leaves every branch it wrote unchanged.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Ok,
                    client: "a",
                    dolt: Some(Dolt::Differs("Dolt's transactions read one snapshot as REPEATABLE READ does, where Postgres' default READ COMMITTED takes one per statement, so the port asks for REPEATABLE READ")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = 20 WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2, 20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (2, 20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET v = 10 WHERE pk = 1;",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    expected_blocking: true,
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' row lock makes the later writer of the row wait for the first to end, and at REPEATABLE READ fail with 40001 once the first commits, so the branch b writes first is the conflicting one, where Dolt's writers never wait and the later committer fails, Appendix C \"transaction concurrency\"")),
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
                    expected: Expected::Tag("ROLLBACK"),
                    client: "a",
                    dolt: Some(Dolt::Differs("the failed statement aborted the transaction, so COMMIT rolls it back, where Dolt's commit merges, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("20")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "rollback discards all dirty branch working sets",
            intent: "ROLLBACK discards a transaction's writes to every branch it wrote through revision names.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, v int);",
                "INSERT INTO t VALUES (1, 0);",
                "SELECT dolt_commit('-Am', 'setup');",
                "SELECT dolt_branch('b1');",
                "SELECT dolt_branch('b2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/main\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b2\".public.t VALUES (2, 20);",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b2\".public.t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "clients can't see changes on other branch heads made since transaction start",
            intent: "A transaction keeps reading another branch's head as it was when the transaction began, until it starts a new transaction.",
            set_up_script: &[
                "CREATE TABLE t1 (a int);",
                "INSERT INTO t1 VALUES (1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Ok,
                    client: "a",
                    dolt: Some(Dolt::Differs("Dolt's transactions read one snapshot as REPEATABLE READ does, where Postgres' default READ COMMITTED takes one per statement, so the port asks for REPEATABLE READ")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM \"postgres/b1\".public.t1;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("a REPEATABLE READ transaction takes its snapshot at its first statement, where Dolt's takes it at START TRANSACTION, so the port reads once first")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (2);",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'new row');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    dolt: Some(Dolt::Differs("Dolt's dolt_commit ends the SQL transaction, so other sessions see its commit at once, where it is part of the transaction until COMMIT, Appendix C \"ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 AS OF 'b1' ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    dolt: Some(Dolt::Differs("MySQL's START TRANSACTION commits the transaction in progress, where Postgres' warns and keeps it, so the port commits first")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 AS OF 'b1' ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_branches table has consistent view",
            intent: "dolt_branches shows a transaction the branches as they were when it began, and another session's branch changes once it starts a new transaction.",
            set_up_script: &[
                "CREATE TABLE t1 (a int);",
                "INSERT INTO t1 VALUES (1);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d', 'b1');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('b2');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("b2")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("b2")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the branches another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "WARNING", code: "25001", message: "there is already a transaction in progress", ..N }],
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("b2")],
                            &[T("main")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_tags name index uses transaction snapshot",
            intent: "Lookups of dolt_tags by name read the transaction's snapshot, so another session's new and deleted tags show only in a later transaction.",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty','-m','original');",
                "SELECT dolt_tag('alpha');",
                "SELECT dolt_tag('beta');",
                "CREATE TABLE wanted (name varchar(100));",
                "INSERT INTO wanted VALUES ('alpha'),('beta'),('gamma'),('delta');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('-d','beta');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('gamma');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tag_name FROM dolt_tags WHERE tag_name = 'beta';",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the tags another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tag_name FROM dolt_tags WHERE tag_name = 'gamma';",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("gamma")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the tags another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tag_name FROM dolt_tags WHERE tag_name between 'alpha' AND 'gamma' ORDER BY tag_name;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("gamma")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the tags another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tag_name FROM dolt_tags WHERE tag_name between 'alpha' AND 'gamma' ORDER BY tag_name;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("gamma")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(w, r) */ r.tag_name FROM wanted w JOIN dolt_tags r ON w.name=r.tag_name ORDER BY r.tag_name;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("gamma")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the tags another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tag_name FROM dolt_tags  ORDER BY tag_name;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("gamma")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the tags another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tag_name FROM dolt_tags WHERE tag_name IN ('alpha','beta','gamma') ORDER BY tag_name;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("gamma")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('-d','gamma');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_tag('delta');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_tag", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: "SELECT tag_name FROM dolt_tags WHERE tag_name between 'alpha' AND 'gamma' ORDER BY tag_name DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("delta")],
                            &[T("alpha")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the tags another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tag_name FROM dolt_tags  ORDER BY tag_name;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("delta")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the tags another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tag_name FROM dolt_tags WHERE tag_name IN ('alpha','beta','gamma','delta') ORDER BY tag_name;",
                    expected: Expected::Rows {
                        columns: &[Column("tag_name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("delta")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dolt_branches name index uses transaction snapshot",
            intent: "Lookups of dolt_branches by name read the transaction's snapshot, so another session's new and deleted branches show only in a later transaction.",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty','-m','original');",
                "SELECT dolt_branch('alpha');",
                "SELECT dolt_branch('beta');",
                "CREATE TABLE wanted (name varchar(100));",
                "INSERT INTO wanted VALUES ('alpha'),('beta'),('gamma'),('delta');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d','beta');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('gamma');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches WHERE name = 'beta';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the branches another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches WHERE name = 'gamma';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("gamma")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the branches another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches WHERE name between 'alpha' AND 'gamma' ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("gamma")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the branches another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches WHERE name between 'alpha' AND 'gamma' ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("gamma")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT /*+ LOOKUP_JOIN(w, r) */ r.name FROM wanted w JOIN dolt_branches r ON w.name=r.name ORDER BY r.name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("gamma")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the branches another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches  ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("gamma")],
                            &[T("main")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the branches another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches WHERE name IN ('alpha','beta','gamma') ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("gamma")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('-d','gamma');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_branch('delta');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
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
                    query: "SELECT name FROM dolt_branches WHERE name between 'alpha' AND 'gamma' ORDER BY name DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("delta")],
                            &[T("alpha")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the branches another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches  ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("delta")],
                            &[T("main")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' READ COMMITTED gives each statement a snapshot of everything committed before it, so the statement sees the branches another session's committed transaction made and deleted, where Dolt's transaction reads the state it started from, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM dolt_branches WHERE name IN ('alpha','beta','gamma','delta') ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("alpha")],
                            &[T("delta")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_multi_db_transaction_tests() {
    run_scripts(&[
        ScriptTest {
            name: "committing to another branch",
            intent: "A transaction's writes to another branch through its revision name commit to that branch, which shows them once checked out.",
            set_up_script: &[
                "CREATE TABLE t1 (a int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT dolt_branch('b1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t1 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t1 VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
            name: "committing to another branch with autocommit",
            intent: "A write outside a transaction block to another branch through its revision name commits to that branch.",
            set_up_script: &[
                "CREATE TABLE t1 (a int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT dolt_branch('b1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t1 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
            name: "committing to another branch with dolt_transaction_commit",
            intent: "With dolt_transaction_commit on, committing a transaction that wrote another branch makes a Dolt commit on that branch.",
            set_up_script: &[
                "CREATE TABLE t1 (a int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT dolt_branch('b1');",
                "BEGIN;",
                "SET dolt_transaction_commit = ON;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t1 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t1 VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1 AS OF 'HEAD' ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM \"postgres/b1\".pg_catalog.dolt_log WHERE message = 'Transaction commit';",
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
                    query: "SELECT count(*) FROM \"postgres/b1\".pg_catalog.dolt_log WHERE message = 'Transaction commit';",
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
                    query: "SELECT * FROM \"postgres/main\".public.t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'b1'")],
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
                    query: "SELECT * FROM \"postgres/b1\".public.t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/main\".public.t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
            name: "committing to another branch with dolt_commit",
            intent: "dolt_commit refuses a transaction that wrote another branch than the current one, and commits after checking that branch out.",
            set_up_script: &[
                "CREATE TABLE t1 (a int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT dolt_branch('b1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t1 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT dolt_t;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'changes on b1');",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "nothing to commit", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT dolt_t;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'b1'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'other changes on b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM dolt_log ORDER BY date DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("other changes on b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "committing to another branch with autocommit and dolt_transaction_commit",
            intent: "With dolt_transaction_commit on, a write outside a transaction block to another branch makes a Dolt commit on that branch.",
            set_up_script: &[
                "CREATE TABLE t1 (a int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT dolt_branch('b1');",
                "SET dolt_transaction_commit = ON;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t1 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1 AS OF 'HEAD' ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM \"postgres/b1\".pg_catalog.dolt_log WHERE message = 'Transaction commit';",
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM \"postgres/b1\".pg_catalog.dolt_log WHERE message = 'Transaction commit';",
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
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'b1'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "25P01", message: "there is no transaction in progress", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
            name: "active_branch with dolt_checkout and use",
            intent: "active_branch() and unqualified reads follow each dolt_checkout inside a transaction that wrote another branch.",
            set_up_script: &[
                "CREATE TABLE t1 (a int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT dolt_branch('b1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t1 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t1 VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Ok,
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
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'b1'")],
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
                            &[T("b1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('main');",
                    expected: Expected::Rows {
                        columns: &[Column("status", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T("0"), T("Switched to branch 'main'")],
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
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
            name: "committing to more than one branch at a time",
            intent: "A transaction that wrote two branches commits both.",
            set_up_script: &[
                "CREATE TABLE t1 (a int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT dolt_branch('b1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO \"postgres/b1\".public.t1 VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "SELECT * FROM \"postgres/main\".public.t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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
            name: "committing to more than one branch at a time with checkout",
            intent: "A transaction that wrote one branch, checked out another, and wrote it commits both.",
            set_up_script: &[
                "CREATE TABLE t1 (a int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'new table');",
                "SELECT dolt_branch('b1');",
                "BEGIN;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_checkout('b1');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
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
                    query: "SELECT * FROM \"postgres/main\".public.t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM \"postgres/b1\".public.t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
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

#[test]
fn test_auto_increment_transaction_tests() {
    run_scripts(&[
        ScriptTest {
            name: "two auto increment values in two transactions",
            intent: "Two transactions inserting into a table with a generated key get different key values.",
            set_up_script: &[
                "CREATE TABLE t (x int PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY, y text);",
                "INSERT INTO t (y) VALUES ('abc123');",
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
                    query: "INSERT INTO t (y) VALUES ('client a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t (y) VALUES ('client b');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", TEXT)],
                        rows: &[
                            &[T("1"), T("abc123")],
                            &[T("2"), T("client a")],
                            &[T("3"), T("client b")],
                        ],
                        tag: "SELECT 3",
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
fn test_dolt_conflict_handling_tests() {
    run_scripts(&[
        ScriptTest {
            name: "default behavior (rollback on commit conflict)",
            intent: "Two transactions insert the same key: the second waits for the first, fails as a duplicate key once the first commits, and its COMMIT rolls back, leaving no conflicts.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "SELECT dolt_commit('-a', '-m', 'initial table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_pkey\"", detail: "Key (pk)=(1) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' unique check waits for the other transaction's uncommitted insert of the key and fails once it commits, where Dolt merges the transactions at commit, Appendix C \"transaction concurrency\"")),
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
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    dolt: Some(Dolt::Differs("the failed statement aborted the transaction, so COMMIT rolls it back, where Dolt's commit merges, Appendix C \"transaction concurrency\"")),
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
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "allow commit conflicts on, conflict on transaction commit",
            intent: "Two transactions insert the same key: the second waits for the first, fails as a duplicate key once the first commits, and its COMMIT rolls back, leaving no conflicts. dolt_allow_commit_conflicts changes nothing here.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "SELECT dolt_commit('-a', '-m', 'initial table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = on;",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = on;",
                    expected: Expected::Tag("SET"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_pkey\"", detail: "Key (pk)=(1) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' unique check waits for the other transaction's uncommitted insert of the key and fails once it commits, where Dolt merges the transactions at commit, Appendix C \"transaction concurrency\"")),
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
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    dolt: Some(Dolt::Differs("the failed statement aborted the transaction, so COMMIT rolls it back, where Dolt's commit merges, Appendix C \"transaction concurrency\"")),
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
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "force commit on, conflict on transaction commit (same as dolt_allow_commit_conflicts)",
            intent: "Two transactions insert the same key: the second waits for the first, fails as a duplicate key once the first commits, and its COMMIT rolls back, leaving no conflicts. dolt_force_transaction_commit changes nothing here.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "SELECT dolt_commit('-a', '-m', 'initial table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = on;",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = on;",
                    expected: Expected::Tag("SET"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 2);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_pkey\"", detail: "Key (pk)=(1) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' unique check waits for the other transaction's uncommitted insert of the key and fails once it commits, where Dolt merges the transactions at commit, Appendix C \"transaction concurrency\"")),
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
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    dolt: Some(Dolt::Differs("the failed statement aborted the transaction, so COMMIT rolls it back, where Dolt's commit merges, Appendix C \"transaction concurrency\"")),
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
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "allow commit conflicts on, conflict on dolt_merge",
            intent: "A merge inside a transaction leaves conflicts that dolt_allow_commit_conflicts lets the transaction commit, and resolving them by hand then commits the resolution.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "SELECT dolt_commit('-a', '-m', 'initial table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = on;",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = on;",
                    expected: Expected::Tag("SET"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'new-branch');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'commit on main');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    dolt: Some(Dolt::Differs("Dolt's dolt_commit ends the SQL transaction, so other sessions see its commit at once, where it is part of the transaction until COMMIT, Appendix C \"ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'commit on new-branch');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    dolt: Some(Dolt::Differs("Dolt's dolt_commit ends the SQL transaction, so other sessions see its commit at once, where it is part of the transaction until COMMIT, Appendix C \"ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('new-branch', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
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
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test t SET val = (SELECT their_val FROM dolt_conflicts_test WHERE our_pk = 1) WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_test;",
                    expected: Expected::Tag("DELETE 1"),
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
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
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
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "force commit on, conflict on dolt_merge (same as dolt_allow_commit_conflicts)",
            intent: "A merge inside a transaction leaves conflicts that dolt_force_transaction_commit lets the transaction commit, and resolving them by hand then commits the resolution.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "SELECT dolt_commit('-a', '-m', 'initial table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = on;",
                    expected: Expected::Tag("SET"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_force_transaction_commit = on;",
                    expected: Expected::Tag("SET"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'new-branch');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'commit on main');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    dolt: Some(Dolt::Differs("Dolt's dolt_commit ends the SQL transaction, so other sessions see its commit at once, where it is part of the transaction until COMMIT, Appendix C \"ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'commit on new-branch');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    dolt: Some(Dolt::Differs("Dolt's dolt_commit ends the SQL transaction, so other sessions see its commit at once, where it is part of the transaction until COMMIT, Appendix C \"ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('new-branch', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
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
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE test t SET val = (SELECT their_val FROM dolt_conflicts_test WHERE our_pk = 1) WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM dolt_conflicts_test;",
                    expected: Expected::Tag("DELETE 1"),
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
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
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
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "allow commit conflicts off, conflict on dolt_merge",
            intent: "A merge inside a transaction leaves conflicts, so the transaction can't commit and rolls back, losing what it wrote after the merge.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY, val int);",
                "SELECT dolt_add('.');",
                "INSERT INTO test VALUES (0, 0);",
                "SELECT dolt_commit('-a', '-m', 'initial table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('-b', 'new-branch');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'commit on main');",
                    expected: Expected::Ok,
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "a",
                    dolt: Some(Dolt::Differs("Dolt's dolt_commit ends the SQL transaction, so other sessions see its commit at once, where it is part of the transaction until COMMIT, Appendix C \"ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'commit on new-branch');",
                    expected: Expected::Ok,
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    dolt: Some(Dolt::Differs("Dolt's dolt_commit ends the SQL transaction, so other sessions see its commit at once, where it is part of the transaction until COMMIT, Appendix C \"ROLLBACK after dolt_commit, dolt_add, dolt_reset, or dolt_checkout in a transaction block\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_preview_merge_conflicts_summary('new-branch', 'main');",
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_data_conflicts", INT8), Column("num_schema_conflicts", INT8)],
                        rows: &[
                            &[T("public.test"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_merge('main');",
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[T(""), T("0"), T("1"), T("conflicts found")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM dolt_conflicts;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts must be resolved using the dolt_conflicts and dolt_schema_conflicts tables before committing a transaction. To commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("val", INT4)],
                        rows: &[
                            &[T("0"), T("0")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "conflicts from a DOLT_MERGE return an initially unhelpful error in a concurrent write scenario",
            intent: "Two transactions on a branch with merge conflicts: the one that allows conflicts commits, and the other fails to commit because of the conflicts, each time it tries.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-am', 'create table');",
                "SELECT dolt_checkout('-b', 'right');",
                "INSERT INTO t VALUES (1, 100);",
                "SELECT dolt_commit('-am', 'right edit');",
                "SELECT dolt_checkout('main');",
                "INSERT INTO t VALUES (1, 200);",
                "SELECT dolt_commit('-am', 'left edit');",
                "SET dolt_allow_commit_conflicts = on;",
                "SELECT dolt_merge('right');",
                "SET dolt_allow_commit_conflicts = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("200"), T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT base_pk, base_col1, our_pk, our_col1, their_pk, their_col1 FROM dolt_conflicts_t;",
                    expected: Expected::Rows {
                        columns: &[Column("base_pk", INT4), Column("base_col1", INT4), Column("our_pk", INT4), Column("our_col1", INT4), Column("their_pk", INT4), Column("their_col1", INT4)],
                        rows: &[
                            &[Null, Null, T("1"), T("200"), T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (2, 2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET dolt_allow_commit_conflicts = on;",
                    expected: Expected::Tag("SET"),
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
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts must be resolved using the dolt_conflicts and dolt_schema_conflicts tables before committing a transaction. To commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    client: "b",
                    dolt: Some(Dolt::Differs("Dolt fails this commit with a retry error and its own comment calls that wrong, since the transaction holds conflicts from the merge that a retry keeps; Doltgrust gives the unresolved-conflicts error that Dolt gives on the retry")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (3, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "Merge conflict detected, transaction rolled back. Merge conflicts must be resolved using the dolt_conflicts and dolt_schema_conflicts tables before committing a transaction. To commit transactions with merge conflicts, set dolt_allow_commit_conflicts to on", ..E }),
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres has no @autocommit or @@ settings, so the message names an explicit transaction and the setting as Postgres sets it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "transaction conflicts follows first-write-wins (a commits first)",
            intent: "Two REPEATABLE READ transactions update the same row: the second writer waits for the first, fails to serialize once the first commits, and rolls back with its other writes.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "CREATE INDEX col1_idx ON t (col1);",
                "CREATE TABLE keyless (col1 int);",
                "INSERT INTO t VALUES (1, 1);",
                "INSERT INTO keyless VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET col1 = -100 WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (1), (1);",
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET col1 = 100 WHERE pk = 1;",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    expected_blocking: true,
                    client: "b",
                    dolt: Some(Dolt::Differs("Postgres' row lock makes the later writer wait for the first to end, and at REPEATABLE READ fail with 40001 once the first commits, so the first to write wins where Dolt's first to commit wins, Appendix C \"transaction concurrency\"")),
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
                    expected: Expected::Tag("ROLLBACK"),
                    client: "b",
                    dolt: Some(Dolt::Differs("the failed statement aborted the transaction, so COMMIT rolls it back, where Dolt's commit merges, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("-100")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t WHERE col1 = -100;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("-100")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "transaction conflicts follows first-write-wins (b commits first)",
            intent: "Two REPEATABLE READ transactions update the same row, b first: a waits for b, fails to serialize once b commits, and rolls back with its other writes, so b's row and inserts stay.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, col1 int);",
                "CREATE INDEX col1_idx ON t (col1);",
                "CREATE TABLE keyless (col1 int);",
                "INSERT INTO t VALUES (1, 1);",
                "INSERT INTO keyless VALUES (1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "START TRANSACTION ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "START TRANSACTION ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("START TRANSACTION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET col1 = 100 WHERE pk = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "a",
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless VALUES (1), (1);",
                    expected: Expected::Tag("INSERT 0 2"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET col1 = -100 WHERE pk = 1;",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    expected_blocking: true,
                    client: "a",
                    dolt: Some(Dolt::Differs("Postgres' row lock makes a, which writes the row after b, wait for b to end and fail with 40001 at REPEATABLE READ once b commits, so b commits first by writing first, Appendix C \"transaction concurrency\"")),
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
                    expected: Expected::Tag("ROLLBACK"),
                    client: "a",
                    dolt: Some(Dolt::Differs("the failed statement aborted the transaction, so COMMIT rolls it back, where Dolt's commit merges, Appendix C \"transaction concurrency\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM keyless;",
                    expected: Expected::Rows {
                        columns: &[Column("col1", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("1")],
                            &[T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t WHERE col1 = 100;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("col1", INT4)],
                        rows: &[
                            &[T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "b",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
