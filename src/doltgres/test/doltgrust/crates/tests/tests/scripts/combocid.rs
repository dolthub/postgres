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

use harness::oid::*;
use harness::pgx::Time;
use harness::plan::PlanFact;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_combocid() {
    run_scripts(&[
        ScriptTest {
            name: "combo command ids, Postgres' combocid.sql",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE combocidtest (foobar int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid,cmin,* FROM combocidtest;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("foobar", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("10"), T("1")],
                            &[T("(0,2)"), T("11"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s1;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE combocidtest SET foobar = foobar + 10;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid,cmin,* FROM combocidtest;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("foobar", INT4)],
                        rows: &[
                            &[T("(0,3)"), T("12"), T("11")],
                            &[T("(0,4)"), T("12"), T("12")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s1;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid,cmin,* FROM combocidtest;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("foobar", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("0"), T("1")],
                            &[T("(0,2)"), T("1"), T("2")],
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
                    query: "SELECT ctid,cmin,* FROM combocidtest;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("foobar", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("0"), T("1")],
                            &[T("(0,2)"), T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    skip: Some("Postgres keeps a tuple header's command field after its transaction ends, which Doltgrust keeps only within the transaction, where cmin and cmax are read (Appendix C: cmin and cmax after a transaction ends)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest VALUES (333);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT ctid,cmin,* FROM combocidtest;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM combocidtest;",
                    expected: Expected::Tag("DELETE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("foobar", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("1")],
                            &[T("(0,2)"), T("1"), T("2")],
                            &[T("(0,5)"), T("0"), T("333")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid,cmin,* FROM combocidtest;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("foobar", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("1")],
                            &[T("(0,2)"), T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    skip: Some("Postgres keeps a tuple header's command field after its transaction ends, which Doltgrust keeps only within the transaction, where cmin and cmax are read (Appendix C: cmin and cmax after a transaction ends)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO combocidtest VALUES (444);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid,cmin,* FROM combocidtest;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("foobar", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("1")],
                            &[T("(0,2)"), T("1"), T("2")],
                            &[T("(0,6)"), T("10"), T("444")],
                        ],
                        tag: "SELECT 3",
                    },
                    skip: Some("Postgres keeps a tuple header's command field after its transaction ends, which Doltgrust keeps only within the transaction, where cmin and cmax are read (Appendix C: cmin and cmax after a transaction ends)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s1;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid,cmin,* FROM combocidtest FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("foobar", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("1")],
                            &[T("(0,2)"), T("1"), T("2")],
                            &[T("(0,6)"), T("10"), T("444")],
                        ],
                        tag: "SELECT 3",
                    },
                    skip: Some("Postgres keeps a tuple header's command field after its transaction ends, which Doltgrust keeps only within the transaction, where cmin and cmax are read (Appendix C: cmin and cmax after a transaction ends)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid,cmin,* FROM combocidtest FOR UPDATE;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid,cmin,* FROM combocidtest;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("foobar", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("1")],
                            &[T("(0,2)"), T("1"), T("2")],
                            &[T("(0,6)"), T("10"), T("444")],
                        ],
                        tag: "SELECT 3",
                    },
                    skip: Some("Postgres keeps a tuple header's command field after its transaction ends, which Doltgrust keeps only within the transaction, where cmin and cmax are read (Appendix C: cmin and cmax after a transaction ends)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE combocidtest SET foobar = foobar + 10;",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid,cmin,* FROM combocidtest;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("foobar", INT4)],
                        rows: &[
                            &[T("(0,7)"), T("12"), T("11")],
                            &[T("(0,8)"), T("12"), T("12")],
                            &[T("(0,9)"), T("12"), T("454")],
                        ],
                        tag: "SELECT 3",
                    },
                    skip: Some("a keyless table's rows are updated in hash order rather than heap order, so its new ctids go to different rows (7263)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s1;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid,cmin,* FROM combocidtest;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("foobar", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("12"), T("1")],
                            &[T("(0,2)"), T("12"), T("2")],
                            &[T("(0,6)"), T("0"), T("444")],
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
                ScriptTestAssertion {
                    query: "SELECT ctid,cmin,* FROM combocidtest;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("cmin", CID), Column("foobar", INT4)],
                        rows: &[
                            &[T("(0,1)"), T("12"), T("1")],
                            &[T("(0,2)"), T("12"), T("2")],
                            &[T("(0,6)"), T("0"), T("444")],
                        ],
                        tag: "SELECT 3",
                    },
                    skip: Some("Postgres keeps a tuple header's command field after its transaction ends, which Doltgrust keeps only within the transaction, where cmin and cmax are read (Appendix C: cmin and cmax after a transaction ends)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE IF NOT EXISTS testcase(id int PRIMARY KEY, balance numeric );",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO testcase VALUES (1, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM testcase WHERE testcase.id = 1 FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("balance", NUMERIC)],
                        rows: &[
                            &[T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE testcase SET balance = balance + 400 WHERE id=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT subxact;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE testcase SET balance = balance - 100 WHERE id=1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT subxact;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM testcase WHERE id = 1 FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("balance", NUMERIC)],
                        rows: &[
                            &[T("1"), T("400")],
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
                ScriptTestAssertion {
                    query: "DROP TABLE testcase;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a combo command id a released subtransaction gave survives the rollback of the savepoint around it",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEMP TABLE rel (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rel VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rel VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT a;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT b;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rel SET a = a + 10 WHERE a = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RELEASE b;",
                    expected: Expected::Tag("RELEASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO a;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT c;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE rel SET a = a + 20 WHERE a = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RELEASE c;",
                    expected: Expected::Tag("RELEASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, cmin FROM rel ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("cmin", CID)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("22"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE r CURSOR FOR SELECT a, cmin FROM rel ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM r;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("cmin", CID)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("22"), T("3")],
                        ],
                        tag: "FETCH 2",
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
            name: "a parallel worker shows the combo command ids of the transaction it reads for",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE pcombo (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pcombo SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pcombo SELECT 1 LIMIT 0;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pcombo VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pcombo VALUES (2);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE pcombo SET a = a + 10;",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL debug_parallel_query = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, cmin FROM pcombo ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Gather")],
                            &[T("  Workers Planned: 1")],
                            &[T("  Single Copy: true")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: a")],
                            &[T("        ->  Seq Scan on pcombo")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, cmin FROM pcombo ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("cmin", CID)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("1")],
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
            ],
            ..S
        },
    ]);
}
