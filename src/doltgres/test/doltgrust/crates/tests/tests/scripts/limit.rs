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
fn test_limit_offset() {
    run_scripts(&[
        ScriptTest {
            name: "basic limit tests",
            set_up_script: &[
                "CREATE TABLE t (i INT PRIMARY KEY, c int)",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3), (4, 4)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t LIMIT 2",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t LIMIT $1",
                    bind_vars: &[BindVar::Int64(2)],
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t LIMIT 2 OFFSET 2",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t order by c asc LIMIT 2 OFFSET 2",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
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
fn test_fetch_first_with_ties() {
    run_scripts(&[
        ScriptTest {
            name: "FETCH FIRST WITH TIES returns the rows that tie with the last, either way",
            set_up_script: &[
                "CREATE TABLE wt (a INT, b TEXT) WITH (autovacuum_enabled = false);",
                "INSERT INTO wt VALUES (1, 'x'), (2, 'y'), (2, 'z'), (2, 'w'), (3, 'v'), (4, 'u'), (4, 't');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT a, b FROM wt ORDER BY a FETCH FIRST 2 ROWS WITH TIES) s ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("w")],
                            &[T("2"), T("y")],
                            &[T("2"), T("z")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM wt ORDER BY a OFFSET 4 ROWS FETCH FIRST 2 ROWS WITH TIES;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
                            &[T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM wt ORDER BY a, b FETCH FIRST 3 ROWS WITH TIES;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("w")],
                            &[T("2"), T("y")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM wt ORDER BY a DESC FETCH FIRST 1 ROW WITH TIES;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM wt ORDER BY a FETCH FIRST 0 ROWS WITH TIES;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM wt FETCH FIRST 2 ROWS WITH TIES;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "WITH TIES cannot be specified without ORDER BY clause", position: 37, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM wt ORDER BY a FETCH FIRST NULL ROWS WITH TIES;",
                    expected: Expected::Error(Diagnostic { code: "2201W", message: "row count cannot be null in FETCH FIRST ... WITH TIES clause", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW wtv AS SELECT a FROM wt ORDER BY a FETCH FIRST 2 ROWS WITH TIES;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT replace(pg_get_viewdef('wtv'::regclass), E'\\n', ' ');",
                    expected: Expected::Rows {
                        columns: &[Column("replace", TEXT)],
                        rows: &[
                            &[T(" SELECT a    FROM wt   ORDER BY a  FETCH FIRST (2) ROWS WITH TIES;")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM wt ORDER BY a FETCH FIRST 2 ROWS WITH TIES;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: a")],
                            &[T("        ->  Seq Scan on wt")],
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
                    query: "DECLARE c SCROLL CURSOR FOR SELECT a FROM wt ORDER BY a FETCH FIRST 2 ROWS WITH TIES;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("2")],
                            &[T("2")],
                        ],
                        tag: "FETCH 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD ALL FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("2")],
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "FETCH 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 3 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("2")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ALL FROM c;",
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
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
