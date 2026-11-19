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
use harness::script::Cell::{Any, Null, Oid, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_cursors() {
    run_scripts(&[
        ScriptTest {
            name: "FETCH and MOVE directions on a SCROLL cursor",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c SCROLL CURSOR FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM c;",
                    expected: Expected::Tag("FETCH 2"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT IN c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("4"), T("d")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("3"), T("c")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FIRST FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("b")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE 2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("4"), T("d")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FORWARD 10 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FORWARD 10 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                            &[T("4"), T("d")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("3"), T("c")],
                            &[T("2"), T("b")],
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE FORWARD 3 IN c;",
                    expected: Expected::Tag("MOVE 3"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE c;",
                    expected: Expected::Tag("MOVE 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE ALL IN c;",
                    expected: Expected::Tag("MOVE 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH -1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE -2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("4"), T("d")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE 0 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("4"), T("d")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 0 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE 0 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE BACKWARD ALL FROM c;",
                    expected: Expected::Tag("MOVE 0"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE c;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE c;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "c" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    flow: Flow::Exec,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cursors without a scroll option can move backward",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE d CURSOR FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM d;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM d;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 1 FROM d;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM d;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                            &[T("3"), T("c")],
                            &[T("4"), T("d")],
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM d;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                            &[T("4"), T("d")],
                            &[T("3"), T("c")],
                            &[T("2"), T("b")],
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 5",
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
            name: "NO SCROLL cursors only move forward",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE n NO SCROLL CURSOR FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE BACKWARD ALL FROM n;",
                    expected: Expected::Tag("MOVE 0"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM n;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE 0 FROM n;",
                    expected: Expected::Tag("MOVE 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 4 FROM n;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("4"), T("d")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM n;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cursor can only scan forward", hint: "Declare it with SCROLL option to enable backward scan.", ..E }),
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
                    query: "DECLARE n NO SCROLL CURSOR FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 2 FROM n;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("b")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 1 FROM n;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cursor can only scan forward", hint: "Declare it with SCROLL option to enable backward scan.", ..E }),
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
            name: "re-fetching the current row with a zero count",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE i CURSOR FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 0 FROM i;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FORWARD 0 FROM i;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM i;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FORWARD 0 FROM i;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 0 FROM i;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE 0 FROM i;",
                    expected: Expected::Tag("MOVE 1"),
                    flow: Flow::Exec,
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
            name: "cursors return the rows that existed when they were declared",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE i CURSOR FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ct VALUES (6, 'f');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM i;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                            &[T("3"), T("c")],
                            &[T("4"), T("d")],
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 5",
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
            name: "cursor queries",
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE u CURSOR FOR VALUES (1), (2);",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL u;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE w CURSOR FOR WITH x AS (SELECT 1 AS y) SELECT * FROM x;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH w;",
                    expected: Expected::Rows {
                        columns: &[Column("y", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE next CURSOR FOR SELECT 2 AS z;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT next;",
                    expected: Expected::Rows {
                        columns: &[Column("z", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DECLARE "Mixed Case" CURSOR FOR SELECT 3;"#,
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"FETCH "Mixed Case";"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH mixed;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "mixed" does not exist"#, ..E }),
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
            name: "cursor errors",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT * FROM ct;",
                    expected: Expected::Error(Diagnostic { code: "25P01", message: "DECLARE CURSOR can only be used in transaction blocks", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH x;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "x" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE x;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "x" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE x;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "x" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE ALL;",
                    expected: Expected::Tag("CLOSE CURSOR ALL"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE a SCROLL NO SCROLL CURSOR FOR SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "42P11", message: "cannot specify both SCROLL and NO SCROLL", ..E }),
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
                    query: "DECLARE a ASENSITIVE INSENSITIVE CURSOR FOR SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "42P11", message: "cannot specify both ASENSITIVE and INSENSITIVE", ..E }),
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
                    query: "DECLARE c CURSOR FOR SELECT * FROM ct;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "42P03", message: r#"cursor "c" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE h CURSOR WITH HOLD FOR SELECT * FROM ct FOR UPDATE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DECLARE CURSOR WITH HOLD ... FOR UPDATE is not supported", detail: "Holdable cursors must be READ ONLY.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE s SCROLL CURSOR FOR SELECT * FROM ct FOR UPDATE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DECLARE SCROLL CURSOR ... FOR UPDATE is not supported", detail: "Scrollable cursors must be READ ONLY.", ..E }),
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
                    query: "DECLARE f CURSOR FOR SELECT 1 / 0;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH f;",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
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
            name: "pg_cursors",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c SCROLL CURSOR FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE d NO SCROLL CURSOR WITHOUT HOLD FOR SELECT 1 AS x;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE e INSENSITIVE CURSOR WITH HOLD FOR SELECT * FROM ct;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, statement, is_holdable, is_binary, is_scrollable, creation_time IS NOT NULL FROM pg_cursors WHERE name <> '' ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("statement", TEXT), Column("is_holdable", BOOL), Column("is_binary", BOOL), Column("is_scrollable", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("c"), T("DECLARE c SCROLL CURSOR FOR SELECT * FROM ct ORDER BY id;"), T("f"), T("f"), T("t"), T("t")],
                            &[T("d"), T("DECLARE d NO SCROLL CURSOR WITHOUT HOLD FOR SELECT 1 AS x;"), T("f"), T("f"), T("f"), T("t")],
                            &[T("e"), T("DECLARE e INSENSITIVE CURSOR WITH HOLD FOR SELECT * FROM ct;"), T("t"), T("f"), T("t"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, statement, is_holdable, is_binary, is_scrollable FROM pg_cursors WHERE name IN ('c', 'd') ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("statement", TEXT), Column("is_holdable", BOOL), Column("is_binary", BOOL), Column("is_scrollable", BOOL)],
                        rows: &[
                            &[T("c"), T("DECLARE c SCROLL CURSOR FOR SELECT * FROM ct ORDER BY id;"), T("f"), T("f"), T("t")],
                            &[T("d"), T("DECLARE d NO SCROLL CURSOR WITHOUT HOLD FOR SELECT 1 AS x;"), T("f"), T("f"), T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, statement FROM pg_cursors WHERE name = '';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T(""), T("SELECT name, statement FROM pg_cursors WHERE name = '';")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "WITH HOLD cursors outlive their transaction",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DECLARE h CURSOR WITH HOLD FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, is_holdable FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("is_holdable", BOOL)],
                        rows: &[
                            &[T("h"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE h2 CURSOR WITH HOLD FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE w CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM h2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, is_holdable FROM pg_cursors WHERE name <> '' ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("is_holdable", BOOL)],
                        rows: &[
                            &[T("h"), T("t")],
                            &[T("h2"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM h2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("b")],
                            &[T("3"), T("c")],
                            &[T("4"), T("d")],
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM h2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH h;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("3"), T("c")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE h3 CURSOR WITH HOLD FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE h;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM pg_cursors WHERE name <> '' ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("h2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 / 0;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM pg_cursors WHERE name <> '' ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("h2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE ALL;",
                    expected: Expected::Tag("CLOSE CURSOR ALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE e CURSOR WITH HOLD FOR SELECT 1 / 0;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cursors in the implicit transaction of a multi-statement query",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT * FROM ct; FETCH 2 FROM c;",
                    expected: Expected::Tag("FETCH 2"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ROLLBACK TO SAVEPOINT closes cursors declared after the savepoint",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE s SCROLL CURSOR FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT sp;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE t CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("3"), T("c")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT sp;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM pg_cursors WHERE name <> '' ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[
                            &[T("s")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("4"), T("d")],
                        ],
                        tag: "FETCH 1",
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
            name: "BINARY cursors",
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE b BINARY CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, is_binary FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT), Column("is_binary", BOOL)],
                        rows: &[
                            &[T("b"), T("t")],
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
            name: "cursors read their rows as they are fetched",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE r CURSOR FOR SELECT 10 / (id - 3) FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM r;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("-5")],
                            &[T("-10")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM r;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
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
            name: "cursors read the data as it was when they were declared",
            set_up_script: &[
                "CREATE TABLE za (id INT4 PRIMARY KEY, v INT4);",
                "CREATE TABLE zb (id INT4 PRIMARY KEY, a_id INT4, w TEXT);",
                "CREATE INDEX zb_a ON zb (a_id);",
                "INSERT INTO za VALUES (1, 10), (2, 20), (3, 30);",
                "INSERT INTO zb VALUES (1, 1, 'x'), (2, 2, 'y'), (3, 3, 'z');",
                "CREATE VIEW zv AS SELECT za.id, zb.w FROM za JOIN zb ON zb.a_id = za.id;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE j CURSOR FOR SELECT za.id, zb.w FROM za JOIN zb ON zb.a_id = za.id ORDER BY za.id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE s CURSOR FOR SELECT id FROM za WHERE v IN (SELECT v FROM za WHERE v > 10) ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE vw CURSOR FOR SELECT * FROM zv ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE cte CURSOR FOR WITH x AS (SELECT id, v FROM za) SELECT id, (SELECT max(v) FROM za) FROM x ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE lk CURSOR FOR SELECT w FROM zb WHERE a_id = 2;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM j;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("w", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE zb SET w = 'changed';",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO za VALUES (4, 40);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO zb VALUES (4, 4, 'new'), (5, 2, 'dup');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM za WHERE id = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM j;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("w", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM vw;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("w", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM cte;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("max", INT4)],
                        rows: &[
                            &[T("1"), T("30")],
                            &[T("2"), T("30")],
                            &[T("3"), T("30")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM lk;",
                    expected: Expected::Rows {
                        columns: &[Column("w", TEXT)],
                        rows: &[
                            &[T("y")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM j;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cursor can only scan forward", hint: "Declare it with SCROLL option to enable backward scan.", ..E }),
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
            name: "WITH HOLD cursors read their remaining rows when their transaction commits",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE h CURSOR WITH HOLD FOR SELECT 10 / (id - 3) FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("-5")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ct VALUES (9, 'z');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("name", TEXT)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ct;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE w SCROLL CURSOR WITH HOLD FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
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
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("b")],
                            &[T("3"), T("c")],
                            &[T("4"), T("d")],
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                            &[T("4"), T("d")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE w2 NO SCROLL CURSOR WITH HOLD FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM w2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM w2;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cursor can only scan forward", hint: "Declare it with SCROLL option to enable backward scan.", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cursor queries with parameters",
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE p CURSOR FOR SELECT $1::INT4 AS x;",
                    bind_vars: &[BindVar::Int(5)],
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH p;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "FETCH 1",
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
            name: "cursors over historical data",
            set_up_script: &[
                "CREATE TABLE hist (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO hist VALUES (1, 'one');",
                "SELECT dolt_commit('-Am', 'first');",
                "INSERT INTO hist VALUES (2, 'two');",
                "UPDATE hist SET v = 'ONE' WHERE id = 1;",
                "SELECT dolt_commit('-am', 'second');",
                "DELETE FROM hist WHERE id = 1;",
                "INSERT INTO hist VALUES (3, 'three');",
                "SELECT dolt_commit('-am', 'third');",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE wh CURSOR WITH HOLD FOR SELECT * FROM hist AS OF 'HEAD~1' ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE h2 CURSOR FOR SELECT * FROM hist AS OF 'HEAD~2' ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE h1 SCROLL CURSOR FOR SELECT * FROM hist AS OF 'HEAD~1' ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE head CURSOR FOR SELECT * FROM hist AS OF 'HEAD' ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE j CURSOR FOR SELECT cur.id, cur.v, old.v FROM hist cur JOIN hist AS OF 'HEAD~1' old ON cur.id = old.id ORDER BY cur.id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE hh CURSOR FOR SELECT id, v FROM dolt_history_hist ORDER BY v, id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO hist VALUES (4, 'four');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "UPDATE hist SET v = 'TWO';",
                    expected: Expected::Tag("UPDATE 3"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'fourth');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM h2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("one")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH 1 FROM h1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("ONE")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM h1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("two")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM h1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("two")],
                            &[T("1"), T("ONE")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM head;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("two")],
                            &[T("3"), T("three")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM j;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("two"), T("two")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM hh;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("ONE")],
                            &[T("1"), T("one")],
                            &[T("3"), T("three")],
                            &[T("2"), T("two")],
                            &[T("2"), T("two")],
                        ],
                        tag: "FETCH 5",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM wh;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("ONE")],
                            &[T("2"), T("two")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cursors over table functions and system tables",
            set_up_script: &[
                "CREATE TABLE tf (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO tf VALUES (1, 'a');",
                "SELECT dolt_commit('-Am', 'first');",
                "INSERT INTO tf VALUES (2, 'b');",
                "SELECT dolt_commit('-am', 'second');",
                "UPDATE tf SET v = 'c' WHERE id = 1;",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE lg CURSOR FOR SELECT message FROM dolt_log() LIMIT 2;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE df CURSOR FOR SELECT to_id, to_v, from_id, from_v, dolt_diff_type FROM dolt_diff('HEAD~1', 'HEAD', 'tf');",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE dw CURSOR FOR SELECT to_id, to_v, from_v, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 'tf');",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE ds CURSOR FOR SELECT table_name, rows_modified FROM dolt_diff_stat('HEAD', 'WORKING');",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE dp CURSOR FOR SELECT table_name, diff_type, statement FROM dolt_patch('HEAD', 'WORKING');",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE st CURSOR FOR SELECT table_name, staged, status FROM dolt_status;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE dd CURSOR FOR SELECT to_id, to_v, from_v, dolt_diff_type FROM dolt_diff_tf WHERE dolt_to_commit = 'WORKING';",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "DECLARE gs SCROLL CURSOR FOR SELECT * FROM generate_series(1, 5);",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "UPDATE tf SET v = 'z' WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_commit('-am', 'third');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM lg;",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("second")],
                            &[T("first")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM df;",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("to_v", TEXT), Column("from_id", INT4), Column("from_v", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("2"), T("b"), Null, Null, T("added")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM dw;",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("to_v", TEXT), Column("from_v", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("c"), T("a"), T("modified")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM ds;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("rows_modified", INT8)],
                        rows: &[
                            &[T("public.tf"), T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM dp;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("diff_type", TEXT), Column("statement", TEXT)],
                        rows: &[
                            &[T("public.tf"), T("data"), T(r#"UPDATE "tf" SET "v"='c' WHERE "id"=1;"#)],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM st;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.tf"), T("f"), T("modified")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH ALL FROM dd;",
                    expected: Expected::Rows {
                        columns: &[Column("to_id", INT4), Column("to_v", TEXT), Column("from_v", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("c"), T("a"), T("modified")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH 3 FROM gs;",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM gs;",
                    expected: Expected::Rows {
                        columns: &[Column("generate_series", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            skip: Some("dolt_diff() names its change column dolt_diff_type (Appendix C, Dolt's own columns of the per-table system tables), where the expectation says diff_type"),
            ..S
        },
        ScriptTest {
            name: "moving and rewinding a SCROLL cursor",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE s SCROLL CURSOR FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 7 FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE -10 FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE ABSOLUTE 0 IN s;",
                    expected: Expected::Tag("MOVE 0"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE 3 FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("4"), T("d")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("3"), T("c")],
                            &[T("2"), T("b")],
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE FORWARD ALL IN s;",
                    expected: Expected::Tag("MOVE 5"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                            &[T("4"), T("d")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE -5 FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE -6 FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM s;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE 0 IN s;",
                    expected: Expected::Tag("MOVE 1"),
                    flow: Flow::Exec,
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
            name: "moving and rewinding a SCROLL WITH HOLD cursor after its transaction commits",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE h SCROLL CURSOR WITH HOLD FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
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
                ScriptTestAssertion {
                    query: "FETCH RELATIVE -1 FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 7 FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE BACKWARD ALL IN h;",
                    expected: Expected::Tag("MOVE 4"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE FORWARD 2 IN h;",
                    expected: Expected::Tag("MOVE 0"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 1 FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE h;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "moving a NO SCROLL WITH HOLD cursor",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DECLARE n NO SCROLL CURSOR WITH HOLD FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE BACKWARD ALL IN n;",
                    expected: Expected::Tag("MOVE 0"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 2 FROM n;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("b")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE 2 FROM n;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("4"), T("d")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE 0 IN n;",
                    expected: Expected::Tag("MOVE 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 4 FROM n;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cursor can only scan forward", hint: "Declare it with SCROLL option to enable backward scan.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE ALL;",
                    expected: Expected::Tag("CLOSE CURSOR ALL"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "moving through a cursor without rows",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE e SCROLL CURSOR FOR SELECT * FROM ct WHERE id > 10;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH e;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM e;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE ALL IN e;",
                    expected: Expected::Tag("MOVE 0"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE -1 FROM e;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 1 FROM e;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE 0 IN e;",
                    expected: Expected::Tag("MOVE 0"),
                    flow: Flow::Exec,
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
            name: "moving a cursor without a scroll option past its last row",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE f CURSOR FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FORWARD 2 FROM f;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE 1 IN f;",
                    expected: Expected::Tag("MOVE 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM f;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("4"), T("d")],
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM f;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE FORWARD 0 IN f;",
                    expected: Expected::Tag("MOVE 0"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE FORWARD 3 IN f;",
                    expected: Expected::Tag("MOVE 0"),
                    flow: Flow::Exec,
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
            name: "scrolling WITH HOLD cursors after their transaction commits",
            set_up_script: &[
                "CREATE TABLE ct (id INT4 PRIMARY KEY, v TEXT);",
                "INSERT INTO ct VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE w SCROLL CURSOR WITH HOLD FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
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
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("2"), T("b")],
                            &[T("3"), T("c")],
                            &[T("4"), T("d")],
                            &[T("5"), T("e")],
                        ],
                        tag: "FETCH 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("5"), T("e")],
                            &[T("4"), T("d")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE w2 NO SCROLL CURSOR WITH HOLD FOR SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM w2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                            &[T("2"), T("b")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM w2;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cursor can only scan forward", hint: "Declare it with SCROLL option to enable backward scan.", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_cursor_positions() {
    run_scripts(&[
        ScriptTest {
            name: "scrollable cursors: a scan of a table without a primary key turns back inside a run of equal rows",
            set_up_script: &[
                "CREATE TABLE k (v INT);",
                "INSERT INTO k VALUES (2), (2), (2), (2), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE k1 SCROLL CURSOR FOR SELECT v FROM k;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 3 FROM k1;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("2")],
                            &[T("2")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM k1;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("2")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 3 FROM k1;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("2")],
                            &[T("2")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 1 FROM k1;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM k1;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("2")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM k1;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("2")],
                            &[T("2")],
                            &[T("2")],
                            &[T("2")],
                        ],
                        tag: "FETCH 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 4 FROM k1;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE -2 FROM k1;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE FORWARD ALL IN k1;",
                    expected: Expected::Tag("MOVE 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM k1;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("2")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 0 FROM k1;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE c SCROLL CURSOR FOR SELECT v FROM k;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on k")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "scrollable cursors: index scans, TID scans, and VALUES read backward",
            set_up_script: &[
                "CREATE TABLE p (a INT PRIMARY KEY, b INT);",
                "INSERT INTO p VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);",
                "CREATE INDEX p_b ON p (b);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE i1 SCROLL CURSOR FOR SELECT a, b FROM p WHERE a > 1 ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM i1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM i1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("20")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM i1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("5"), T("50")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 3 FROM i1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("4"), T("40")],
                            &[T("3"), T("30")],
                            &[T("2"), T("20")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE i2 SCROLL CURSOR FOR SELECT a FROM p WHERE a < 5 ORDER BY a DESC;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM i2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("3")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM i2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM i2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM i2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "FETCH 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE t1 SCROLL CURSOR FOR SELECT a FROM p WHERE ctid IN ('(0,1)', '(0,3)', '(0,4)');",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("3")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM t1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE t2 SCROLL CURSOR FOR SELECT a FROM p WHERE ctid > '(0,1)' AND ctid < '(0,5)';",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("3")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE v1 SCROLL CURSOR FOR VALUES (1), (2), (3);",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM v1;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM v1;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM v1;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM v1;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("1")],
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
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE c SCROLL CURSOR FOR SELECT a FROM p WHERE ctid IN ('(0,1)', '(0,3)');",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on p")],
                            &[T(r#"  Filter: (ctid = ANY ('{"(0,1)","(0,3)"}'::tid[]))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE c SCROLL CURSOR FOR VALUES (1), (2);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Values Scan on "*VALUES*""#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "scrollable cursors: Append, SubqueryScan, Sort, Limit, and Result read backward, and Materialize goes over a plan that cannot",
            set_up_script: &[
                "CREATE TABLE p (a INT PRIMARY KEY, b INT);",
                "INSERT INTO p VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);",
                "CREATE TABLE pt (a INT PRIMARY KEY) PARTITION BY RANGE (a);",
                "CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (10) TO (20);",
                "INSERT INTO pt VALUES (1), (2), (11), (12);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE a1 SCROLL CURSOR FOR SELECT a FROM pt;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 3 FROM a1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("11")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM a1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM a1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("11")],
                            &[T("12")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM a1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("12")],
                            &[T("11")],
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "FETCH 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE s1 SCROLL CURSOR FOR SELECT x FROM (SELECT a + 1 AS x FROM p OFFSET 0) sub;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM s1;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM s1;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM s1;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE m1 SCROLL CURSOR FOR SELECT a, count(*) FROM p GROUP BY a ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM m1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM m1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM m1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("5"), T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM m1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("4"), T("1")],
                            &[T("3"), T("1")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE o1 SCROLL CURSOR FOR SELECT a FROM p ORDER BY b DESC;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 3 FROM o1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("4")],
                            &[T("3")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM o1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("5")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM o1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("3")],
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "FETCH 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE l1 SCROLL CURSOR FOR SELECT a FROM p ORDER BY b LIMIT 3 OFFSET 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM l1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM l1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("3")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM l1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE -1 FROM l1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 0 FROM l1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM l1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE r1 SCROLL CURSOR FOR SELECT 1 AS one;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH r1;",
                    expected: Expected::Rows {
                        columns: &[Column("one", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM r1;",
                    expected: Expected::Rows {
                        columns: &[Column("one", INT4)],
                        rows: &[
                        ],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM r1;",
                    expected: Expected::Rows {
                        columns: &[Column("one", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM r1;",
                    expected: Expected::Rows {
                        columns: &[Column("one", INT4)],
                        rows: &[
                        ],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM r1;",
                    expected: Expected::Rows {
                        columns: &[Column("one", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE c SCROLL CURSOR FOR SELECT a FROM pt;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Append")],
                            &[T("  ->  Seq Scan on pt1 pt_1")],
                            &[T("  ->  Seq Scan on pt2 pt_2")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE c SCROLL CURSOR FOR SELECT a, count(*) FROM p GROUP BY a ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Materialize")],
                            &[T("  ->  GroupAggregate")],
                            &[T("        Group Key: a")],
                            &[T("        ->  Index Only Scan using p_pkey on p")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE c SCROLL CURSOR FOR SELECT a FROM p ORDER BY b DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort")],
                            &[T("  Sort Key: b DESC")],
                            &[T("  ->  Seq Scan on p")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE c SCROLL CURSOR FOR SELECT a FROM p ORDER BY b LIMIT 3 OFFSET 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: b")],
                            &[T("        ->  Seq Scan on p")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE c SCROLL CURSOR FOR SELECT x FROM (SELECT a + 1 AS x FROM p OFFSET 0) sub;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on p")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE c SCROLL CURSOR FOR SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Materialize")],
                            &[T("  ->  Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE c CURSOR FOR SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE c SCROLL CURSOR FOR SELECT count(*) FROM p;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Materialize")],
                            &[T("  ->  Aggregate")],
                            &[T("        ->  Seq Scan on p")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cursors: NO SCROLL, FOR UPDATE, and the options DECLARE refuses",
            set_up_script: &[
                "CREATE TABLE p (a INT PRIMARY KEY, b INT);",
                "INSERT INTO p VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE n NO SCROLL CURSOR FOR SELECT a FROM p ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM n;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM n;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cursor can only scan forward", hint: "Declare it with SCROLL option to enable backward scan.", ..E }),
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
                    query: "DECLARE u CURSOR FOR SELECT a FROM p ORDER BY a FOR UPDATE;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM u;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 1 FROM u;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cursor can only scan forward", hint: "Declare it with SCROLL option to enable backward scan.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM u;",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 1 FROM u;",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
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
                    query: "DECLARE s SCROLL CURSOR FOR SELECT a FROM p FOR UPDATE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DECLARE SCROLL CURSOR ... FOR UPDATE is not supported", detail: "Scrollable cursors must be READ ONLY.", ..E }),
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
                    query: "DECLARE s SCROLL NO SCROLL CURSOR FOR SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "42P11", message: "cannot specify both SCROLL and NO SCROLL", ..E }),
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
                    query: "DECLARE s ASENSITIVE INSENSITIVE CURSOR FOR SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "42P11", message: "cannot specify both ASENSITIVE and INSENSITIVE", ..E }),
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
                    query: "DECLARE s INSENSITIVE CURSOR FOR SELECT a FROM p FOR SHARE;",
                    expected: Expected::Error(Diagnostic { code: "42P11", message: "DECLARE INSENSITIVE CURSOR ... FOR SHARE is not valid", detail: "Insensitive cursors must be READ ONLY.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE h CURSOR WITH HOLD FOR SELECT a FROM p FOR UPDATE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "DECLARE CURSOR WITH HOLD ... FOR UPDATE is not supported", detail: "Holdable cursors must be READ ONLY.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE nb CURSOR FOR SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "25P01", message: "DECLARE CURSOR can only be used in transaction blocks", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DECLARE "" CURSOR WITH HOLD FOR SELECT 1;"#,
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"zero-length delimited identifier at or near """""#, position: 9, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM missing;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "missing" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE missing;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "missing" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE d CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE d CURSOR FOR SELECT 2;",
                    expected: Expected::Error(Diagnostic { code: "42P03", message: r#"cursor "d" already exists"#, ..E }),
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
            name: "cursors WITH HOLD: kept past COMMIT with their rows unchanged, dropped by the ROLLBACK of their transaction",
            set_up_script: &[
                "CREATE TABLE p (a INT PRIMARY KEY, b INT);",
                "INSERT INTO p VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE h CURSOR WITH HOLD FOR SELECT a, b FROM p ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
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
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (6, 60);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET b = 0 WHERE a = 4;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("30")],
                            &[T("4"), T("40")],
                            &[T("5"), T("50")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("5"), T("50")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE h;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE w SCROLL CURSOR WITH HOLD FOR SELECT a FROM p ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
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
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 1 FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM w;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("5")],
                            &[T("4")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE w;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE x CURSOR WITH HOLD FOR SELECT a FROM p ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT FROM x;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR FROM x;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE r CURSOR WITH HOLD FOR SELECT a FROM p;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM r;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "r" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE e CURSOR WITH HOLD FOR SELECT a FROM p ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 / 0;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM e;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "e" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 / 0;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM x;",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM x;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                        ],
                        tag: "FETCH 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE a CURSOR WITH HOLD FOR SELECT a FROM p ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                        ],
                        tag: "FETCH 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE ALL;",
                    expected: Expected::Tag("CLOSE CURSOR ALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM x;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "x" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "WHERE CURRENT OF: UPDATE and DELETE of a cursor's row",
            set_up_script: &[
                "CREATE TABLE p (a INT PRIMARY KEY, b INT);",
                "INSERT INTO p VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT * FROM p ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET b = 0 WHERE CURRENT OF c;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM p ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("0")],
                            &[T("3"), T("30")],
                            &[T("4"), T("40")],
                            &[T("5"), T("50")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM p WHERE CURRENT OF c RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("3"), T("30")],
                            &[T("4"), T("40")],
                            &[T("5"), T("50")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET b = 1 WHERE CURRENT OF c;",
                    expected: Expected::Error(Diagnostic { code: "24000", message: r#"cursor "c" is not positioned on a row"#, ..E }),
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
                    query: "DECLARE f CURSOR FOR SELECT a FROM p FOR UPDATE;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM f;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET b = -1 WHERE CURRENT OF f RETURNING a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("-1")],
                        ],
                        tag: "UPDATE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE g CURSOR FOR SELECT count(*) FROM p;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH g;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET b = 2 WHERE CURRENT OF g;",
                    expected: Expected::Error(Diagnostic { code: "24000", message: r#"cursor "g" is not a simply updatable scan of table "p""#, ..E }),
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
                    query: "DECLARE n CURSOR FOR SELECT a FROM p;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET b = 3 WHERE CURRENT OF n;",
                    expected: Expected::Error(Diagnostic { code: "24000", message: r#"cursor "n" is not positioned on a row"#, ..E }),
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
                    query: "UPDATE p SET b = 4 WHERE CURRENT OF missing;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "missing" does not exist"#, ..E }),
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
                    query: "DECLARE h CURSOR WITH HOLD FOR SELECT a FROM p ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
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
                    query: "FETCH 1 FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE p SET b = 5 WHERE CURRENT OF h;",
                    expected: Expected::Error(Diagnostic { code: "24000", message: r#"cursor "h" is held from a previous transaction"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE p SET b = 6 WHERE CURRENT OF c;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Update on p")],
                            &[T("  ->  Tid Scan on p")],
                            &[T("        TID Cond: CURRENT OF c")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "cursors WITH HOLD across a dolt_checkout of another branch",
            set_up_script: &[
                "CREATE TABLE p (a INT PRIMARY KEY, b INT);",
                "INSERT INTO p VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);",
                "SELECT dolt_commit('-Am', 'p');",
                "SELECT dolt_branch('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE h CURSOR WITH HOLD FOR SELECT a FROM p ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM p WHERE a > 3;",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'other'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM h;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                        ],
                        tag: "FETCH 4",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_close_all_and_pg_cursors_order() {
    run_scripts(&[
        ScriptTest {
            name: "CLOSE ALL drops cursors in the portal table's order until a pinned one, the order pg_cursors lists",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE alpha CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE beta CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE gamma CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE delta CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE epsilon CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE zeta CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE eta CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE theta CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT string_agg(name, ',') FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[
                            &[T("epsilon,eta,alpha,delta,gamma,beta,theta,zeta")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r record; BEGIN FOR r IN SELECT 1 LOOP BEGIN EXECUTE 'CLOSE ALL'; EXCEPTION WHEN invalid_cursor_state THEN RAISE NOTICE '% / %', SQLERRM, (SELECT string_agg(name, ',') FROM pg_cursors WHERE name <> ''); END; END LOOP; END $$;",
                    notices: &[Diagnostic { code: "00000", message: "cannot drop pinned portal \"<unnamed portal 1>\" / <unnamed portal 1>,gamma,beta,theta,zeta", context: Some("PL/pgSQL function inline_code_block line 1 at RAISE"), ..N }],
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT string_agg(name, ',') FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[
                            &[T("gamma,beta,theta,zeta")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE beta;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE beta CURSOR FOR SELECT 2;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE iota CURSOR FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT string_agg(name, ',') FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[
                            &[T("iota,gamma,beta,theta,zeta")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE ALL;",
                    expected: Expected::Tag("CLOSE CURSOR ALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_cursors WHERE name <> '';",
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
                    query: "DO $$ BEGIN FOR i IN 1..20 LOOP EXECUTE format('DECLARE c%s CURSOR FOR SELECT %s', i, i); END LOOP; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT string_agg(name, ',') FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[
                            &[T("c5,c20,c9,c11,c10,c19,c4,c18,c15,c3,c7,c12,c6,c14,c1,c13,c17,c8,c2,c16")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE c3;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE c17;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c3 CURSOR FOR SELECT 3;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT string_agg(name, ',') FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[
                            &[T("c5,c20,c9,c11,c10,c19,c4,c18,c15,c7,c12,c3,c6,c14,c1,c13,c8,c2,c16")],
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
                    query: "SELECT count(*) FROM pg_cursors WHERE name <> '';",
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
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE held1 CURSOR WITH HOLD FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE held2 CURSOR WITH HOLD FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE held3 CURSOR WITH HOLD FOR SELECT 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT string_agg(name, ',') FROM pg_cursors WHERE name <> '';",
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[
                            &[T("held2,held3,held1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE ALL;",
                    expected: Expected::Tag("CLOSE CURSOR ALL"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_cursors WHERE name <> '';",
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
fn test_where_current_of_after_the_transaction_moved_the_row() {
    run_scripts(&[
        ScriptTest {
            name: "WHERE CURRENT OF follows a row the transaction's updates moved to another key, as Postgres' TID scan follows its newest version",
            set_up_script: &[
                "CREATE TABLE cof_keyless (v int4, w text);",
                "INSERT INTO cof_keyless VALUES (1, 'a'), (2, 'b'), (3, 'c');",
                "CREATE TABLE cof_keyed (id int4 PRIMARY KEY, v int4);",
                "INSERT INTO cof_keyed VALUES (1, 10), (2, 20), (3, 30);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c1 CURSOR FOR SELECT * FROM cof_keyless WHERE v = 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH c1;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("w", TEXT)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cof_keyless SET v = v + 10 WHERE CURRENT OF c1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cof_keyless SET w = w || '!' WHERE CURRENT OF c1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cof_keyless ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("w", TEXT)],
                        rows: &[
                            &[T("2"), T("b")],
                            &[T("3"), T("c")],
                            &[T("11"), T("a!")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM cof_keyless WHERE CURRENT OF c1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cof_keyless ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("w", TEXT)],
                        rows: &[
                            &[T("2"), T("b")],
                            &[T("3"), T("c")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c3 CURSOR FOR SELECT * FROM cof_keyless WHERE v = 2;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH c3;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("w", TEXT)],
                        rows: &[
                            &[T("2"), T("b")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cof_keyless SET v = v * 100 WHERE v = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM cof_keyless WHERE CURRENT OF c3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cof_keyless ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4), Column("w", TEXT)],
                        rows: &[
                            &[T("3"), T("c")],
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
                    query: "DECLARE c2 CURSOR FOR SELECT * FROM cof_keyed ORDER BY id FOR UPDATE;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH c2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cof_keyed SET id = id + 100 WHERE CURRENT OF c2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cof_keyed SET v = v + 1 WHERE CURRENT OF c2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cof_keyed ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                            &[T("101"), T("11")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM cof_keyed WHERE CURRENT OF c2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM cof_keyed ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
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
                    query: "CREATE TEMP TABLE cof_tids AS SELECT ctid AS t FROM cof_keyless WHERE v = 3;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM cof_keyless WHERE ctid = (SELECT t FROM cof_tids);",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cof_keyless SET v = 33 WHERE v = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM cof_keyless WHERE ctid = (SELECT t FROM cof_tids);",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM cof_keyless WHERE w = 'c';",
                    expected: Expected::Rows {
                        columns: &[Column("v", INT4)],
                        rows: &[
                            &[T("33")],
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
            ],
            ..S
        },
        ScriptTest {
            name: "open cursors and running queries keep DDL off the relations they use",
            set_up_script: &[
                "CREATE TABLE ref_t (a INT PRIMARY KEY, b INT);",
                "CREATE INDEX ref_i ON ref_t (b);",
                "INSERT INTO ref_t VALUES (1, 1), (2, 2);",
                "CREATE VIEW ref_v AS SELECT a FROM ref_t;",
                "CREATE FUNCTION ref_f() RETURNS INT LANGUAGE plpgsql AS $$ BEGIN ALTER TABLE ref_t ADD COLUMN e INT; RETURN 1; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE ref_c CURSOR FOR SELECT * FROM ref_t;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM ref_c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ref_t ADD COLUMN c INT;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: r#"cannot ALTER TABLE "ref_t" because it is being used by active queries in this session"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "TRUNCATE ref_t;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: r#"cannot TRUNCATE "ref_t" because it is being used by active queries in this session"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE ref_t CASCADE;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: r#"cannot DROP TABLE "ref_t" because it is being used by active queries in this session"#, ..E }),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to view ref_v", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE ref_t;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE ref_c;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ref_t ADD COLUMN c INT;",
                    expected: Expected::Tag("ALTER TABLE"),
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
                    query: "DECLARE ref_vc CURSOR FOR SELECT * FROM ref_v;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE VIEW ref_v AS SELECT a, 1 AS one FROM ref_t;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ref_t ADD COLUMN d INT;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: r#"cannot ALTER TABLE "ref_t" because it is being used by active queries in this session"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref_f() FROM ref_t;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: r#"cannot ALTER TABLE "ref_t" because it is being used by active queries in this session"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ref_f();",
                    expected: Expected::Rows {
                        columns: &[Column("ref_f", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ DECLARE r RECORD; BEGIN FOR r IN SELECT * FROM ref_t LOOP ALTER TABLE ref_t ADD COLUMN g INT; END LOOP; END $$;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: r#"cannot ALTER TABLE "ref_t" because it is being used by active queries in this session"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET LOCAL enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE ref_ic CURSOR FOR SELECT b FROM ref_t WHERE b = 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX ref_i;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: r#"cannot DROP INDEX "ref_i" because it is being used by active queries in this session"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE ref_t;",
                    expected: Expected::Error(Diagnostic { code: "55006", message: r#"cannot REINDEX INDEX "ref_i" because it is being used by active queries in this session"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s;",
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
                    query: "DECLARE ref_h CURSOR WITH HOLD FOR SELECT a FROM ref_t ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ref_t ADD COLUMN h INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM ref_h;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE ref_h;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE ref_p AS SELECT * FROM ref_t;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE ref_t ADD COLUMN p INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DEALLOCATE ref_p;",
                    expected: Expected::Tag("DEALLOCATE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_where_current_of_over_index_only_scans_of_copies() {
    run_scripts(&[
        ScriptTest {
            name: "WHERE CURRENT OF over an index-only scan of a table without a primary key changes the copy the cursor is on, as Postgres' does by the index entry's tuple id",
            set_up_script: &[
                "CREATE TABLE cio_copies (a int4, b int4);",
                "CREATE INDEX cio_copies_a ON cio_copies (a);",
                "INSERT INTO cio_copies VALUES (1, 0), (1, 0), (1, 0);",
                "CREATE TABLE cio_two (a int4, b int4);",
                "CREATE INDEX cio_two_a ON cio_two (a);",
                "CREATE TABLE cio_marks (n text, x text);",
                "BEGIN;",
                "INSERT INTO cio_two VALUES (1, 0);",
                "INSERT INTO cio_marks VALUES ('first', pg_current_xact_id()::text);",
                "COMMIT;",
                "BEGIN;",
                "INSERT INTO cio_two VALUES (1, 0);",
                "INSERT INTO cio_marks VALUES ('second', pg_current_xact_id()::text);",
                "COMMIT;",
                "SET enable_seqscan = off;",
                "SET enable_bitmapscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM cio_copies WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using cio_copies_a on cio_copies")],
                            &[T("  Index Cond: (a = 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c1 CURSOR FOR SELECT a FROM cio_copies WHERE a = 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH c1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM cio_copies WHERE CURRENT OF c1;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH c1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cio_copies SET b = 1 WHERE CURRENT OF c1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, a, b FROM cio_copies ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("(0,3)"), T("1"), T("0")],
                            &[T("(0,4)"), T("1"), T("1")],
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
                    query: "DECLARE c2 CURSOR FOR SELECT a FROM cio_two WHERE a = 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH c2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH c2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM cio_two WHERE CURRENT OF c2;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, a, xmin::text = (SELECT x FROM cio_marks WHERE n = 'first') AS first FROM cio_two ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("first", BOOL)],
                        rows: &[
                            &[T("(0,1)"), T("1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c3 SCROLL CURSOR FOR SELECT a FROM cio_two WHERE a = 1;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO cio_two VALUES (1, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL c3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL c3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH NEXT c3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cio_two SET b = 5 WHERE CURRENT OF c3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ctid, a, b, xmin::text = (SELECT x FROM cio_marks WHERE n = 'first') AS first FROM cio_two ORDER BY ctid;",
                    expected: Expected::Rows {
                        columns: &[Column("ctid", TID), Column("a", INT4), Column("b", INT4), Column("first", BOOL)],
                        rows: &[
                            &[T("(0,3)"), T("1"), T("0"), T("f")],
                            &[T("(0,4)"), T("1"), T("5"), T("f")],
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

#[test]
fn test_cursor_that_failed_in_a_rolled_back_subtransaction() {
    run_scripts(&[
        ScriptTest {
            name: "a cursor whose FETCH failed in a subtransaction that rolled back stays, failed, while one declared in it goes",
            set_up_script: &[
                "CREATE TABLE fail_cur (u int);",
                "INSERT INTO fail_cur SELECT generate_series(1, 30);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c CURSOR FOR SELECT u / 0 FROM fail_cur ORDER BY u;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT two;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 10 FROM c;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: r#"division by zero"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT two;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 10 FROM c;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"portal "c" cannot be run"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO SAVEPOINT two;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RELEASE SAVEPOINT two;",
                    expected: Expected::Tag("RELEASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 10 FROM c;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: r#"portal "c" cannot be run"#, ..E }),
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
                    query: "DECLARE ok CURSOR FOR SELECT u FROM fail_cur ORDER BY u;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SAVEPOINT s1;",
                    expected: Expected::Tag("SAVEPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE foo CURSOR FOR SELECT u FROM fail_cur ORDER BY u;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ok;",
                    expected: Expected::Rows {
                        columns: &[Column("u", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 / 0;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: r#"division by zero"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK TO s1;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ok;",
                    expected: Expected::Rows {
                        columns: &[Column("u", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH foo;",
                    expected: Expected::Error(Diagnostic { code: "34000", message: r#"cursor "foo" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_where_current_of_over_index_only_scans_moving_both_ways() {
    run_scripts(&[
        ScriptTest {
            name: "WHERE CURRENT OF over an index-only scan of a table with a primary key changes the row the cursor is on after it fetches and moves in either direction, as Postgres' does",
            set_up_script: &[
                "CREATE TABLE cio_keyed (id int4 PRIMARY KEY, a int4, b int4);",
                "CREATE INDEX cio_keyed_a ON cio_keyed (a);",
                "INSERT INTO cio_keyed SELECT i, i * 10, 0 FROM generate_series(1, 5) i;",
                "VACUUM ANALYZE cio_keyed;",
                "SET enable_seqscan = off;",
                "SET enable_bitmapscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM cio_keyed ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using cio_keyed_a on cio_keyed")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM cio_keyed WHERE a > 15 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using cio_keyed_a on cio_keyed")],
                            &[T("  Index Cond: (a > 15)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c SCROLL CURSOR FOR SELECT a FROM cio_keyed ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("20")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 1 c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("10")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cio_keyed SET b = 1 WHERE CURRENT OF c;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE 2 c;",
                    expected: Expected::Tag("MOVE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cio_keyed SET b = 2 WHERE CURRENT OF c;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH PRIOR c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("20")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM cio_keyed WHERE CURRENT OF c;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("50")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cio_keyed SET b = 3 WHERE CURRENT OF c;",
                    expected: Expected::Tag("UPDATE 1"),
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
                    query: "DECLARE d CURSOR FOR SELECT a FROM cio_keyed WHERE a > 15 ORDER BY a;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 d;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("30")],
                            &[T("40")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cio_keyed SET b = 4 WHERE CURRENT OF d;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, b FROM cio_keyed ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("10"), T("1")],
                            &[T("3"), T("30"), T("2")],
                            &[T("4"), T("40"), T("4")],
                            &[T("5"), T("50"), T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "WHERE CURRENT OF over an index-only scan of a GiST index changes the row the cursor is on, as Postgres' does",
            set_up_script: &[
                "CREATE TABLE cio_points (id int4 PRIMARY KEY, p point, b int4);",
                "CREATE INDEX cio_points_p ON cio_points USING gist (p);",
                "INSERT INTO cio_points SELECT i, point(i, i), 0 FROM generate_series(1, 5) i;",
                "VACUUM ANALYZE cio_points;",
                "SET enable_seqscan = off;",
                "SET enable_bitmapscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT p FROM cio_points WHERE p <@ box '((1.5,1.5),(2.5,2.5))';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using cio_points_p on cio_points")],
                            &[T("  Index Cond: (p <@ '(2.5,2.5),(1.5,1.5)'::box)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE g CURSOR FOR SELECT p FROM cio_points WHERE p <@ box '((1.5,1.5),(2.5,2.5))';",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH g;",
                    expected: Expected::Rows {
                        columns: &[Column("p", POINT)],
                        rows: &[
                            &[T("(2,2)")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE cio_points SET b = 1 WHERE CURRENT OF g;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, b FROM cio_points ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("0")],
                            &[T("2"), T("1")],
                            &[T("3"), T("0")],
                            &[T("4"), T("0")],
                            &[T("5"), T("0")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_cursors_over_a_table_without_a_primary_key_turning_back_from_a_row() {
    run_scripts(&[
        ScriptTest {
            name: "a cursor over a table without a primary key that turns back from the row it is on returns the rows before it, so FETCH BACKWARD and FETCH RELATIVE 0 give Postgres' rows",
            set_up_script: &[
                "CREATE TABLE fbk (f1 int, f2 text);",
                "INSERT INTO fbk VALUES (5, 'five');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c SCROLL CURSOR FOR SELECT * FROM fbk;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4), Column("f2", TEXT)],
                        rows: &[
                            &[T("5"), T("five")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4), Column("f2", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4), Column("f2", TEXT)],
                        rows: &[
                            &[T("5"), T("five")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE 0 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4), Column("f2", TEXT)],
                        rows: &[
                            &[T("5"), T("five")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4), Column("f2", TEXT)],
                        rows: &[],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4), Column("f2", TEXT)],
                        rows: &[
                            &[T("5"), T("five")],
                        ],
                        tag: "FETCH 1",
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
                    query: "DECLARE u CURSOR FOR SELECT * FROM fbk;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH u;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4), Column("f2", TEXT)],
                        rows: &[
                            &[T("5"), T("five")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE fbk SET f1 = 6 WHERE CURRENT OF u;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE 0 FROM u;",
                    expected: Expected::Rows {
                        columns: &[Column("f1", INT4), Column("f2", TEXT)],
                        rows: &[
                            &[T("5"), T("five")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM fbk WHERE CURRENT OF u;",
                    expected: Expected::Tag("DELETE 1"),
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

#[test]
fn test_cursors_over_an_inheritance_parent_rewound_to_an_earlier_child() {
    run_scripts(&[
        ScriptTest {
            name: "WHERE CURRENT OF on a cursor over an inheritance parent that was rewound to an earlier child changes only the row the cursor is on",
            set_up_script: &[
                "CREATE TABLE cinh (a int);",
                "CREATE TABLE cinh_1 () INHERITS (cinh);",
                "CREATE TABLE cinh_2 () INHERITS (cinh);",
                "INSERT INTO cinh_1 VALUES (1);",
                "INSERT INTO cinh_2 VALUES (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c SCROLL CURSOR FOR SELECT * FROM cinh;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM cinh WHERE CURRENT OF c RETURNING a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "DELETE 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM cinh;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
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
    ]);
}
